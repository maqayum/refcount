all: htmref graph

CFLAGS =
WARNINGS = -Wall -Werror
DEBUG = -ggdb
OPTIMIZE = -O2
LIBS = -lpthread

htmref: htmref.c
	$(CC) -mrtm -o $@ $(CFLAGS) $(WARNINGS) $(DEBUG) $(OPTIMIZE) $(LIBS) $^

graph: graph.c
	$(CC) -o $@ $(CFLAGS) $(WARNINGS) $(DEBUG) $(OPTIMIZE) $(LIBS) $^

clean:
	rm -f htmref graph
