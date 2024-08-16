## Clone ToG repo
```
git clone https://github.com/IDEA-FinAI/ToG.git
cd Freebase
```

## Download Freebase data
Download freebase dump and unzip it.
```
wget https://commondatastorage.googleapis.com/freebase-public/rdf/freebase-rdf-latest.gz
gunzip -c freebase-rdf-latest.gz > freebase # data size: 400G
```

Download python script to clean the data and remove non-English or non-digital triplets.
```
wget https://github.com/lanyunshi/Multi-hopComplexKBQA/blob/master/code/FreebaseTool/FilterEnglishTriplets.py
nohup python -u FilterEnglishTriplets.py 0<freebase 1>FilterFreebase 2>log_err & # data size: 125G
```

Download OpenLink Virtuoso 7.2.5 and start service.
```
wget https://sourceforge.net/projects/virtuoso/files/virtuoso/7.2.5/virtuoso-opensource.x86_64-generic_glibc25-linux-gnu.tar.gz
tar xvpfz virtuoso-opensource.x86_64-generic_glibc25-linux-gnu.tar.gz
cd virtuoso-opensource/database/
mv virtuoso.ini.sample virtuoso.ini

# ../bin/virtuoso-t -df # start the service in the shell
../bin/virtuoso-t  # start the service in the backend.
../bin/isql 1111 dba dba # run the database

# 1、unzip the data and use rdf_loader to import
SQL>
ld_dir('.', 'FilterFreebase', 'http://freebase.com'); 
rdf_loader_run(); 
```

However, the `ld_dir` and `rdf_loader_run()` seems to fail.

### Requirements for testing.
```
pip3 install SPARQLWrapper
```
### Testing
Save test example below as test.py
```
import json
from SPARQLWrapper import SPARQLWrapper, JSON

SPARQLPATH = "http://localhost:8890/sparql"

def test():
    try:
        sparql = SPARQLWrapper(SPARQLPATH)
        sparql_txt = """PREFIX ns: <http://rdf.freebase.com/ns/>
            SELECT distinct ?name3
            WHERE {
            ns:m.0k2kfpc ns:award.award_nominated_work.award_nominations ?e1.
            ?e1 ns:award.award_nomination.award_nominee ns:m.02pbp9.
            ns:m.02pbp9 ns:people.person.spouse_s ?e2.
            ?e2 ns:people.marriage.spouse ?e3.
            ?e2 ns:people.marriage.from ?e4.
            ?e3 ns:type.object.name ?name3
            MINUS{?e2 ns:type.object.name ?name2}
            }
        """
        #print(sparql_txt)
        sparql.setQuery(sparql_txt)
        sparql.setReturnFormat(JSON)
        results = sparql.query().convert()
        print(results)
    except:
        print('Your database is not installed properly !!!')

test()

```

## Move to ToG/ToG
```
cd root/ToG/ToG
```

### requirements.

```
pip3 install tqdm openai rank_bm25 sentence_transformers bs4
```

### Run
```
python3 main_freebase.py --max_length 256 --temperature_exploration 0.4 --temperature_reasoning 0 --width 3 --depth 3 --remove_unnecessary_rel True --LLM_type llama --num_retain_entity 5 --prune_tools llm
```