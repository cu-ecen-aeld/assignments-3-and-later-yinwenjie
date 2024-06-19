#? /bin/bash
file_path=$1
write_str=$2

if [ "$#" -lt 2 ]; then
    echo "Error: missing arguments!"
    exit 1
fi
if [ -w ${file_path} ]; then
   echo ${write_str} > ${file_path}
else
   dir_path=$(dirname ${file_path})
   mkdir -p ${dir_path}
   echo ${write_str} > ${file_path} 
fi
