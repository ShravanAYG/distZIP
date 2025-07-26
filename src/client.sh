#!/bin/bash

nc -l -p 9999 > tmp.bin
tail -n+2 tmp.bin > $(head -n1 tmp.bin | awk '{printf $3 ".tmp"}')
UUID=$(head -n1 tmp.bin | awk '{printf $3}')
FILENAME=$(head -n1 tmp.bin | awk '{printf $1}')
FILESIZE=$(head -n1 tmp.bin | awk '{printf $2}')
IPADDR=$(head -n1 tmp.bin | awk '{printf $4}')

echo "INSERT INTO files (uuid, file_name, file_size, ip_address) VALUES ('$UUID', '$FILENAME', $FILESIZE, '$IPADDR');" | sqlite3 myfiles.db

echo "SELECT * FROM files WHERE uuid = '$UUID';" | sqlite3 myfiles.db
ls

gzip $UUID.tmp
sleep 2
sed -i "s/$UUID.tmp/$FILENAME/g" "$UUID.tmp.gz"
echo $FILENAME.gz $UUID $(wc -c $UUID.tmp.gz | awk '{printf $1}') | cat - $UUID.tmp.gz | nc -q 1 $IPADDR 9998
echo "DELETE FROM files WHERE uuid = '$UUID';" | sqlite3 myfiles.db
rm $UUID.tmp.gz

