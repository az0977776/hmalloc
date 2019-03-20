# 
CFLAGS := -g -Wall -Werror -std=gnu99
SRCS   := $(wildcard *.c)
# 

all:
	#(cd tests && make)  

clean:
	#(cd tests && make clean)
	rm -f valgrind.out stdout.txt stderr.txt *.plist 


