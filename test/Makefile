# Host build of the radio-independent logic.  No ESP-IDF required.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -DARGUS_HOST_TEST
INCS     = -I../main
SRCS     = test_detect.c ../main/argus_detect.c ../main/argus_track.c \
           ../main/argus_mute.c

run: $(SRCS) ../main/argus_oui_table.h
	$(CC) $(CFLAGS) $(INCS) $(SRCS) -o run

../main/argus_oui_table.h:
	cd .. && python3 tools/gen_oui_table.py

.PHONY: test clean
test: run
	./run

clean:
	rm -f run
