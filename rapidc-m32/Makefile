CC = cc
CFLAGS = -Wall -Wextra -g -Isrc
SRC = src

all: rapidc runtime.o

$(SRC)/lexer.yy.c: $(SRC)/lexer.l $(SRC)/parser.tab.h
	flex -o $(SRC)/lexer.yy.c $(SRC)/lexer.l

$(SRC)/parser.tab.c $(SRC)/parser.tab.h: $(SRC)/parser.y
	bison -d -o $(SRC)/parser.tab.c $(SRC)/parser.y

rapidc: $(SRC)/main.c $(SRC)/ast.c $(SRC)/semantic.c $(SRC)/codegen.c $(SRC)/var_struct_map.c $(SRC)/module.c $(SRC)/parser.tab.c $(SRC)/lexer.yy.c
	$(CC) $(CFLAGS) -o rapidc $(SRC)/main.c $(SRC)/ast.c $(SRC)/semantic.c $(SRC)/codegen.c $(SRC)/var_struct_map.c $(SRC)/module.c $(SRC)/parser.tab.c $(SRC)/lexer.yy.c

# M5: runtime.c is compiled to a .o once and linked into every program rapidc produces
runtime.o: $(SRC)/runtime.c
	$(CC) $(CFLAGS) -c -o runtime.o $(SRC)/runtime.c

clean:
	rm -f rapidc $(SRC)/lexer.yy.c $(SRC)/parser.tab.c $(SRC)/parser.tab.h

.PHONY: all clean
