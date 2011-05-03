testname=$1
echo "
  EXECUTE SCRIPT (
       SET ID = 1,
       FILENAME = '${testname}/ddl_updates.sql',
       EVENT NODE = 1,
       lock='public.table4,public.billing_discount'
    );

  try {
       execute script (
            set id=1,
            filename='${testname}/bad_ddl.sql',
            event node=1,
            lock='public.snobol'
       );
  } on error {
     echo 'a bad DDL script gets rolled back on the master';
  } on success {
     echo 'a bad DDL script did not get rolled back - ERROR!';
     exit -1;
  }
"
