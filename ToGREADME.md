```
git clone https://github.com/IDEA-FinAI/ToG.git
cd Freebase
```

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

