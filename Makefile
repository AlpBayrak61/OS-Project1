CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -O2

all: mysumfix

mysumfix: mysumfix.cpp
	$(CC) $(CFLAGS) mysumfix.cpp -o mysumfix

clean:
	rm -f mysumfix

