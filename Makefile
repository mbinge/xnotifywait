INCLUDE= #-framework CoreServices
CFLAGS+= #-Wincompatible-pointer-types
OBJECTS+=xnotifywait.o

all: xnotifywait 

xnotifywait: $(OBJECTS)
	gcc $(INCLUDE) $(CFLAGS) -o xnotifywait $(OBJECTS)

clean:
	rm -f *.o xnotifywait
