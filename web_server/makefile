CC = gcc
CFLAGS = -pthread -O2 -Wall -Wextra
DEPS = functions.h structs.h
OBJ = functions.o web_server.o

%.o : %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

web_server : $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

.PHONY : clean

clean :
	-rm -f $(OBJ) core *~
