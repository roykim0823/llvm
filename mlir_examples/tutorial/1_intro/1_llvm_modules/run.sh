
llc -filetype=obj --relocation-model=pic simple.ll -o simple.o
clang -shared -fPIC simple.o -o libsimple.so
clang simple.o -o simple # optionally create an executable
./simple; echo $?

python3 simple.py
rm simple simple.o libsimple.so
