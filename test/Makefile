# Host build of the radio-independent logic.  No ESP-IDF required.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -DOBSERVORE_HOST_TEST
INCS     = -I../main
SRCS     = test_detect.c ../main/observore_detect.c ../main/observore_track.c \
           ../main/observore_mute.c ../main/observore_util.c \
           ../main/observore_notify_fmt.c ../main/observore_wps.c

run: $(SRCS) ../main/observore_oui_table.h
	$(CC) $(CFLAGS) $(INCS) $(SRCS) -o run

../main/observore_oui_table.h:
	cd .. && python3 tools/gen_oui_table.py

.PHONY: test clean
test: run
	./run

clean:
	rm -f run
