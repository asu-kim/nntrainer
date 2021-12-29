// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2021 hyeonseok lee <hs89.lee@samsung.com>
 *
 * @file   grucell.cpp
 * @date   28 Oct 2021
 * @brief  This is Gated Recurrent Unit Cell Layer Class of Neural Network
 * @see    https://github.com/nnstreamer/nntrainer
 * @author hyeonseok lee <hs89.lee@samsung.com>
 * @bug    No known bugs except for NYI items
 *
 * h_prev --------d1------->[*]-------d0----->[+]---d0--> h
 * d_h_prev |  |             |                 | d0      dh
 *          | d14            | d2        d3    |
 *          |  |             +-----[1-]------>[*]
 *          | [*]<---+ d15   |d5               | d6
 *          |  |     |reset_g| update_gate     | memory_cell
 *          |  |    [sig]   [sig]            [tanh]
 *          |  |     |d16    | d7              |d8
 *          |  |    [+]      [+]              [+]
 *          |  |    / \d16   |  \ d7          / \ d8
 *          |  |  Whhr Wxhr Whhz Wxhz       Whhg Wxhg
 *          |  |  |d17  |d13 |d12 |d11       |d10 | d9
 *          +- |--+------|---+    |          |    |
 *             +---------|--------|----------+    |
 *   xs------------------+--------+---------------+
 */

#include <cmath>

#include <grucell.h>
#include <lazy_tensor.h>
#include <nntrainer_error.h>
#include <nntrainer_log.h>
#include <node_exporter.h>
#include <util_func.h>

#include <layer_context.h>

namespace nntrainer {

static constexpr size_t SINGLE_INOUT_IDX = 0;

enum GRUCellParams {
  weight_ih,
  weight_hh,
  bias_h,
  bias_ih,
  bias_hh,
  hidden_state,
  zrg,
  dropout_mask
};

// Todo: handle with strided tensor more efficiently and reduce temporary
// tensors
GRUCellLayer::GRUCellLayer() :
  LayerImpl(),
  grucell_props(props::Unit(),
                props::HiddenStateActivation() = ActivationType::ACT_TANH,
                props::RecurrentActivation() = ActivationType::ACT_SIGMOID,
                props::DropOutRate(), props::IntegrateBias(),
                props::ResetAfter(), props::MaxTimestep(), props::Timestep()),
  acti_func(ActivationType::ACT_NONE, true),
  recurrent_acti_func(ActivationType::ACT_NONE, true),
  epsilon(1e-3) {
  wt_idx.fill(std::numeric_limits<unsigned>::max());
}

void GRUCellLayer::finalize(InitLayerContext &context) {
  const Tensor::Initializer weight_initializer =
    std::get<props::WeightInitializer>(*layer_impl_props).get();
  const Tensor::Initializer bias_initializer =
    std::get<props::BiasInitializer>(*layer_impl_props).get();
  const WeightRegularizer weight_regularizer =
    std::get<props::WeightRegularizer>(*layer_impl_props).get();
  const float weight_regularizer_constant =
    std::get<props::WeightRegularizerConstant>(*layer_impl_props).get();
  const bool disable_bias =
    std::get<props::DisableBias>(*layer_impl_props).get();

  const unsigned int unit = std::get<props::Unit>(grucell_props).get();
  const bool integrate_bias =
    std::get<props::IntegrateBias>(grucell_props).get();
  const ActivationType hidden_state_activation_type =
    std::get<props::HiddenStateActivation>(grucell_props).get();
  const ActivationType recurrent_activation_type =
    std::get<props::RecurrentActivation>(grucell_props).get();
  const float dropout_rate = std::get<props::DropOutRate>(grucell_props).get();
  const unsigned int max_timestep =
    std::get<props::MaxTimestep>(grucell_props).get();

  if (context.getNumInputs() != 1) {
    throw std::invalid_argument("GRUCell layer takes only one input");
  }

  // input_dim = [ batch_size, 1, 1, feature_size ]
  const TensorDim &input_dim = context.getInputDimensions()[0];
  if (input_dim.channel() != 1 && input_dim.height() != 1) {
    throw std::invalid_argument(
      "Input must be single time dimension for GRUCell");
  }

  const unsigned int batch_size = input_dim.batch();
  const unsigned int feature_size = input_dim.width();

  // output_dim = [ batch_size, 1, 1, unit ]
  TensorDim output_dim(batch_size, 1, 1, unit);
  context.setOutputDimensions({output_dim});

  // weight_initializer can be set seperately. weight_ih initializer,
  // weight_hh initializer kernel initializer & recurrent_initializer in keras
  // for now, it is set same way.

  // - weight_ih ( input to hidden )
  // weight_ih_dim : [ 1, 1, feature_size, NUMGATE * unit ] -> z, r, g
  TensorDim weight_ih_dim({feature_size, NUM_GATE * unit});
  wt_idx[GRUCellParams::weight_ih] =
    context.requestWeight(weight_ih_dim, weight_initializer, weight_regularizer,
                          weight_regularizer_constant, "weight_ih", true);
  // - weight_hh ( hidden to hidden )
  // weight_hh_dim : [ 1, 1, unit, NUM_GATE * unit ] -> z, r, g
  TensorDim weight_hh_dim({unit, NUM_GATE * unit});
  wt_idx[GRUCellParams::weight_hh] =
    context.requestWeight(weight_hh_dim, weight_initializer, weight_regularizer,
                          weight_regularizer_constant, "weight_hh", true);
  if (!disable_bias) {
    if (integrate_bias) {
      // - bias_h ( input bias, hidden bias are integrate to 1 bias )
      // bias_h_dim : [ 1, 1, 1, NUM_GATE * unit ] -> z, r, g
      TensorDim bias_h_dim({NUM_GATE * unit});
      wt_idx[GRUCellParams::bias_h] =
        context.requestWeight(bias_h_dim, bias_initializer,
                              WeightRegularizer::NONE, 1.0f, "bias_h", true);
    } else {
      // - bias_ih ( input bias )
      // bias_ih_dim : [ 1, 1, 1, NUM_GATE * unit ] -> z, r, g
      TensorDim bias_ih_dim({NUM_GATE * unit});
      wt_idx[GRUCellParams::bias_ih] =
        context.requestWeight(bias_ih_dim, bias_initializer,
                              WeightRegularizer::NONE, 1.0f, "bias_ih", true);
      // - bias_hh ( hidden bias )
      // bias_hh_dim : [ 1, 1, 1, NUM_GATE * unit ] -> z, r, g
      TensorDim bias_hh_dim({NUM_GATE * unit});
      wt_idx[GRUCellParams::bias_hh] =
        context.requestWeight(bias_hh_dim, bias_initializer,
                              WeightRegularizer::NONE, 1.0f, "bias_hh", true);
    }
  }

  // hidden_state_dim = [ max_timestep * batch_size, 1, 1, unit ]
  TensorDim hidden_state_dim(max_timestep * batch_size, 1, 1, unit);
  wt_idx[GRUCellParams::hidden_state] = context.requestTensor(
    hidden_state_dim, "hidden_state", Tensor::Initializer::NONE, true,
    TensorLifespan::ITERATION_LIFESPAN, false);

  // zrg_dim = [ batch_size, 1, 1, NUM_GATE * unit ]
  TensorDim zrg_dim(batch_size, 1, 1, NUM_GATE * unit);
  wt_idx[GRUCellParams::zrg] =
    context.requestTensor(zrg_dim, "zrg", Tensor::Initializer::NONE, true,
                          TensorLifespan::ITERATION_LIFESPAN);

  if (dropout_rate > epsilon) {
    // dropout_mask_dim = [ batch_size, 1, 1, unit ]
    TensorDim dropout_mask_dim(batch_size, 1, 1, unit);
    wt_idx[GRUCellParams::dropout_mask] = context.requestTensor(
      dropout_mask_dim, "dropout_mask", Tensor::Initializer::NONE, false,
      TensorLifespan::ITERATION_LIFESPAN);
  }

  acti_func.setActiFunc(hidden_state_activation_type);
  recurrent_acti_func.setActiFunc(recurrent_activation_type);
}

void GRUCellLayer::setProperty(const std::vector<std::string> &values) {
  auto remain_props = loadProperties(values, grucell_props);
  LayerImpl::setProperty(remain_props);
}

void GRUCellLayer::exportTo(Exporter &exporter,
                            const ExportMethods &method) const {
  LayerImpl::exportTo(exporter, method);
  exporter.saveResult(grucell_props, method, this);
}

void GRUCellLayer::forwarding(RunLayerContext &context, bool training) {
  const bool disable_bias =
    std::get<props::DisableBias>(*layer_impl_props).get();

  const unsigned int unit = std::get<props::Unit>(grucell_props).get();
  const bool integrate_bias =
    std::get<props::IntegrateBias>(grucell_props).get();
  const bool reset_after = std::get<props::ResetAfter>(grucell_props).get();
  const float dropout_rate = std::get<props::DropOutRate>(grucell_props).get();
  const unsigned int max_timestep =
    std::get<props::MaxTimestep>(grucell_props).get();
  const unsigned int timestep = std::get<props::Timestep>(grucell_props).get();

  const Tensor &input = context.getInput(SINGLE_INOUT_IDX);
  Tensor &output = context.getOutput(SINGLE_INOUT_IDX);
  const unsigned int batch_size = input.getDim().batch();

  const Tensor &weight_ih = context.getWeight(wt_idx[GRUCellParams::weight_ih]);
  const Tensor &weight_hh = context.getWeight(wt_idx[GRUCellParams::weight_hh]);
  Tensor empty;
  const Tensor &bias_h = !disable_bias && integrate_bias
                           ? context.getWeight(wt_idx[GRUCellParams::bias_h])
                           : empty;
  const Tensor &bias_ih = !disable_bias && !integrate_bias
                            ? context.getWeight(wt_idx[GRUCellParams::bias_ih])
                            : empty;
  const Tensor &bias_hh = !disable_bias && !integrate_bias
                            ? context.getWeight(wt_idx[GRUCellParams::bias_hh])
                            : empty;

  Tensor &hidden_states =
    context.getTensor(wt_idx[GRUCellParams::hidden_state]);
  hidden_states.reshape({max_timestep, 1, batch_size, unit});
  Tensor prev_hidden_state;
  if (!timestep) {
    prev_hidden_state = Tensor(batch_size, unit);
    prev_hidden_state.setZero();
  } else {
    prev_hidden_state = hidden_states.getBatchSlice(timestep - 1, 1);
  }
  prev_hidden_state.reshape({batch_size, 1, 1, unit});
  Tensor hidden_state = hidden_states.getBatchSlice(timestep, 1);
  hidden_state.reshape({batch_size, 1, 1, unit});

  Tensor &zrg = context.getTensor(wt_idx[GRUCellParams::zrg]);

  input.dot(weight_ih, zrg);

  Tensor update_reset_gate =
    zrg.getSharedDataTensor({batch_size, 1, 1, 2 * unit}, 0, false);
  Tensor memory_cell =
    zrg.getSharedDataTensor({batch_size, 1, 1, unit}, 2 * unit, false);

  Tensor weight_hh_update_reset_gate;
  Tensor weight_hh_memory_cell;
  weight_hh_update_reset_gate.copy_with_stride(
    weight_hh.getSharedDataTensor({unit, 2 * unit}, 0, false));
  weight_hh_memory_cell.copy_with_stride(
    weight_hh.getSharedDataTensor({unit, unit}, 2 * unit, false));

  update_reset_gate.add_i_strided(
    prev_hidden_state.dot(weight_hh_update_reset_gate));
  if (!disable_bias) {
    if (integrate_bias) {
      const Tensor bias_h_update_reset_gate =
        bias_h.getSharedDataTensor({2 * unit}, 0);
      update_reset_gate.add_i(bias_h_update_reset_gate);
    } else {
      const Tensor bias_ih_update_reset_gate =
        bias_ih.getSharedDataTensor({2 * unit}, 0);
      update_reset_gate.add_i(bias_ih_update_reset_gate);
      const Tensor bias_hh_update_reset_gate =
        bias_hh.getSharedDataTensor({2 * unit}, 0);
      update_reset_gate.add_i(bias_hh_update_reset_gate);
    }
  }

  recurrent_acti_func.run_fn(update_reset_gate, update_reset_gate);

  Tensor update_gate =
    update_reset_gate.getSharedDataTensor({batch_size, 1, 1, unit}, 0, false);
  Tensor reset_gate = update_reset_gate.getSharedDataTensor(
    {batch_size, 1, 1, unit}, unit, false);

  Tensor temp;
  if (reset_after) {
    prev_hidden_state.dot(weight_hh_memory_cell, temp);
    if (!disable_bias && !integrate_bias) {
      const Tensor bias_hh_memory_cell =
        bias_hh.getSharedDataTensor({unit}, 2 * unit);
      temp.add_i(bias_hh_memory_cell);
    }
    temp.multiply_i_strided(reset_gate);
    memory_cell.add_i_strided(temp);
  } else {
    reset_gate.multiply_strided(prev_hidden_state, temp);
    temp.dot(weight_hh_memory_cell, memory_cell, false, false, 1.0f);
    if (!disable_bias && !integrate_bias) {
      const Tensor bias_hh_memory_cell =
        bias_hh.getSharedDataTensor({unit}, 2 * unit);
      memory_cell.add_i(bias_hh_memory_cell);
    }
  }
  if (!disable_bias) {
    if (integrate_bias) {
      const Tensor bias_h_memory_cell =
        bias_h.getSharedDataTensor({unit}, 2 * unit);
      memory_cell.add_i(bias_h_memory_cell);
    } else {
      const Tensor bias_ih_memory_cell =
        bias_ih.getSharedDataTensor({unit}, 2 * unit);
      memory_cell.add_i(bias_ih_memory_cell);
    }
  }

  acti_func.run_fn(memory_cell, memory_cell);

  update_gate.multiply_strided(prev_hidden_state, hidden_state);
  temp = update_gate.multiply(-1.0).add(1.0);
  hidden_state.add_i(memory_cell.multiply_strided(temp));

  if (dropout_rate > epsilon && training) {
    Tensor mask = context.getTensor(wt_idx[GRUCellParams::dropout_mask]);
    mask.dropout_mask(dropout_rate);
    hidden_state.multiply_i(mask);
  }

  output.copyData(hidden_state);
}

void GRUCellLayer::calcDerivative(RunLayerContext &context) {
  Tensor &outgoing_derivative = context.getOutgoingDerivative(SINGLE_INOUT_IDX);
  const Tensor &weight_ih = context.getWeight(wt_idx[GRUCellParams::weight_ih]);
  const Tensor &d_zrg = context.getTensorGrad(wt_idx[GRUCellParams::zrg]);

  d_zrg.dot(weight_ih, outgoing_derivative, false, true);
}

void GRUCellLayer::calcGradient(RunLayerContext &context) {
  const bool disable_bias =
    std::get<props::DisableBias>(*layer_impl_props).get();

  const unsigned int unit = std::get<props::Unit>(grucell_props).get();
  const bool integrate_bias =
    std::get<props::IntegrateBias>(grucell_props).get();
  const bool reset_after = std::get<props::ResetAfter>(grucell_props).get();
  const float dropout_rate = std::get<props::DropOutRate>(grucell_props).get();
  const unsigned int max_timestep =
    std::get<props::MaxTimestep>(grucell_props).get();
  const unsigned int timestep = std::get<props::Timestep>(grucell_props).get();

  const Tensor &input = context.getInput(SINGLE_INOUT_IDX);
  const unsigned int batch_size = input.getDim().batch();

  Tensor &d_weight_ih = context.getWeightGrad(wt_idx[GRUCellParams::weight_ih]);
  const Tensor &weight_hh = context.getWeight(wt_idx[GRUCellParams::weight_hh]);
  Tensor &d_weight_hh = context.getWeightGrad(wt_idx[GRUCellParams::weight_hh]);

  Tensor empty;
  Tensor &d_bias_h = !disable_bias && integrate_bias
                       ? context.getWeightGrad(wt_idx[GRUCellParams::bias_h])
                       : empty;
  Tensor &d_bias_ih = !disable_bias && !integrate_bias
                        ? context.getWeightGrad(wt_idx[GRUCellParams::bias_ih])
                        : empty;
  const Tensor &bias_hh = !disable_bias && !integrate_bias
                            ? context.getWeight(wt_idx[GRUCellParams::bias_hh])
                            : empty;
  Tensor &d_bias_hh = !disable_bias && !integrate_bias
                        ? context.getWeightGrad(wt_idx[GRUCellParams::bias_hh])
                        : empty;

  Tensor d_weight_hh_update_reset_gate =
    d_weight_hh.getSharedDataTensor({unit, 2 * unit}, 0, false);
  Tensor d_weight_hh_memory_cell =
    d_weight_hh.getSharedDataTensor({unit, unit}, 2 * unit, false);
  Tensor &hidden_states =
    context.getTensor(wt_idx[GRUCellParams::hidden_state]);
  hidden_states.reshape({max_timestep, 1, batch_size, unit});
  Tensor &d_hidden_states =
    context.getTensorGrad(wt_idx[GRUCellParams::hidden_state]);
  const Tensor &incoming_derivative =
    context.getIncomingDerivative(SINGLE_INOUT_IDX);
  const Tensor &zrg = context.getTensor(wt_idx[GRUCellParams::zrg]);
  Tensor &d_zrg = context.getTensorGrad(wt_idx[GRUCellParams::zrg]);

  d_hidden_states.reshape({max_timestep, 1, batch_size, unit});
  Tensor d_hidden_state = d_hidden_states.getBatchSlice(timestep, 1);
  d_hidden_state.reshape({batch_size, 1, 1, unit});
  if (timestep + 1 == max_timestep) {
    d_weight_ih.setZero();
    d_weight_hh.setZero();
    if (!disable_bias) {
      if (integrate_bias) {
        d_bias_h.setZero();
      } else {
        d_bias_ih.setZero();
        d_bias_hh.setZero();
      }
    }
    d_hidden_state.setZero();
  }

  d_hidden_state.add_i(incoming_derivative);

  Tensor prev_hidden_state;
  Tensor d_prev_hidden_state;
  if (timestep) {
    prev_hidden_state = hidden_states.getBatchSlice(timestep - 1, 1);
    d_prev_hidden_state = d_hidden_states.getBatchSlice(timestep - 1, 1);
  } else {
    prev_hidden_state = Tensor(batch_size, unit);
    prev_hidden_state.setZero();
    d_prev_hidden_state = Tensor(batch_size, unit);
    d_prev_hidden_state.setZero();
  }
  prev_hidden_state.reshape({batch_size, 1, 1, unit});
  d_prev_hidden_state.reshape({batch_size, 1, 1, unit});

  if (dropout_rate > epsilon) {
    d_hidden_states.multiply_i(
      context.getTensor(wt_idx[GRUCellParams::dropout_mask]));
  }

  Tensor update_gate =
    zrg.getSharedDataTensor({batch_size, 1, 1, unit}, 0, false);
  Tensor reset_gate =
    zrg.getSharedDataTensor({batch_size, 1, 1, unit}, unit, false);
  Tensor memory_cell =
    zrg.getSharedDataTensor({batch_size, 1, 1, unit}, 2 * unit, false);

  Tensor d_update_gate =
    d_zrg.getSharedDataTensor({batch_size, 1, 1, unit}, 0, false);
  Tensor d_reset_gate =
    d_zrg.getSharedDataTensor({batch_size, 1, 1, unit}, unit, false);
  Tensor d_memory_cell =
    d_zrg.getSharedDataTensor({batch_size, 1, 1, unit}, 2 * unit, false);

  d_hidden_state.multiply_strided(
    update_gate, d_prev_hidden_state); // d_prev_hidden_state = d1
  d_hidden_state.multiply_strided(prev_hidden_state,
                                  d_update_gate); // d_update_gate = d2
  d_update_gate.add_i_strided(d_hidden_state.multiply_strided(memory_cell),
                              -1.0f); // d_update_gate = d5
  update_gate.multiply(-1.0, d_memory_cell);
  d_memory_cell.add_i(1.0);
  d_memory_cell.multiply_i_strided(d_hidden_state); // d_memory_cell = d6

  recurrent_acti_func.run_prime_fn(update_gate, d_update_gate,
                                   d_update_gate); // d_update_gate = d7
  acti_func.run_prime_fn(memory_cell, d_memory_cell,
                         d_memory_cell); // d_memory_cell = d8

  Tensor d_update_reset_gate = d_zrg.getSharedDataTensor(
    {batch_size, 1, 1, 2 * unit}, 0, false); // d_update_gate+d_reset_gate

  Tensor weight_hh_memory_cell;
  weight_hh_memory_cell.copy_with_stride(
    weight_hh.getSharedDataTensor({unit, unit}, 2 * unit, false));
  Tensor weight_hh_update_reset_gate;
  weight_hh_update_reset_gate.copy_with_stride(
    weight_hh.getSharedDataTensor({unit, 2 * unit}, 0, false));

  Tensor temp = Tensor(batch_size, 1, 1, unit);
  Tensor d_memory_cell_contiguous;
  d_memory_cell_contiguous.copy_with_stride(d_memory_cell);

  if (reset_after) {
    prev_hidden_state.dot(weight_hh_memory_cell, temp);
    if (!disable_bias && !integrate_bias) {
      const Tensor bias_hh_memory_cell =
        bias_hh.getSharedDataTensor({unit}, 2 * unit);
      temp.add_i(bias_hh_memory_cell);
    }
    d_memory_cell_contiguous.multiply_strided(
      temp, d_reset_gate); // d_reset_gate = d15

    // reset temp: d_memory_cell_contiguous * reset_gate for
    // d_bias_hh_memory_cell, d_prev_hidden_state and d_weight_hh_memory_cell
    d_memory_cell_contiguous.multiply_strided(reset_gate, temp);
    if (!disable_bias && !integrate_bias) {
      Tensor d_bias_hh_memory_cell =
        d_bias_hh.getSharedDataTensor({unit}, 2 * unit);
      temp.sum(0, d_bias_hh_memory_cell, 1.0, 1.0);
    }
    temp.dot(weight_hh_memory_cell, d_prev_hidden_state, false, true,
             1.0); // d_prev_hidden_state = d1 + d14
    d_weight_hh_memory_cell.add_i_strided(
      prev_hidden_state.dot(temp, true, false));
  } else {
    if (!disable_bias && !integrate_bias) {
      Tensor d_bias_hh_memory_cell =
        d_bias_hh.getSharedDataTensor({unit}, 2 * unit);
      d_memory_cell.sum(0, d_bias_hh_memory_cell, 1.0, 1.0);
    }

    d_memory_cell_contiguous.dot(weight_hh_memory_cell, temp, false, true);
    temp.multiply_strided(prev_hidden_state, d_reset_gate);
    temp.multiply_strided(reset_gate, d_prev_hidden_state, 1.0f);

    // reset temp: reset_gate * prev_hidden_state for and
    // d_weight_hh_memory_cell
    reset_gate.multiply_strided(prev_hidden_state, temp);
    temp.dot(d_memory_cell_contiguous, d_weight_hh_memory_cell, true, false,
             1.0f);
  }

  recurrent_acti_func.run_prime_fn(reset_gate, d_reset_gate,
                                   d_reset_gate); // d_reset_gate = d16

  if (!disable_bias) {
    if (integrate_bias) {
      d_zrg.sum(0, d_bias_h, 1.0, 1.0);
    } else {
      d_zrg.sum(0, d_bias_ih, 1.0, 1.0);
      Tensor d_bias_hh_update_reset_gate =
        d_bias_hh.getSharedDataTensor({2 * unit}, 0);
      d_bias_hh_update_reset_gate.add_i(
        d_zrg.sum(0).getSharedDataTensor({2 * unit}, 0));
    }
  }

  Tensor d_update_reset_gate_contiguous;
  d_update_reset_gate_contiguous.copy_with_stride(d_update_reset_gate);
  d_weight_hh_update_reset_gate.add_i_strided(
    prev_hidden_state.dot(d_update_reset_gate_contiguous, true, false));
  input.dot(d_zrg, d_weight_ih, true, false, 1.0f);
  d_update_reset_gate_contiguous.dot(
    weight_hh_update_reset_gate, d_prev_hidden_state, false, true,
    1.0); // d_prev_hidden_state = d1 + d14 + d12 + d17
}

void GRUCellLayer::setBatch(RunLayerContext &context, unsigned int batch) {
  const float dropout_rate = std::get<props::DropOutRate>(grucell_props);
  unsigned int &max_timestep = std::get<props::MaxTimestep>(grucell_props);
  context.updateTensor(wt_idx[GRUCellParams::hidden_state],
                       max_timestep * batch);
  context.updateTensor(wt_idx[GRUCellParams::zrg], batch);
  if (dropout_rate > epsilon) {
    context.updateTensor(wt_idx[GRUCellParams::dropout_mask], batch);
  }
}

} // namespace nntrainer
