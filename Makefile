# Makefile for CS-4760 Project 4 - Process Scheduling
# Author: Cynthia Brown
# Date: 2026-04-07

CC      = gcc
CFLAGS  = -Wall -g

TARGETS = oss user

.SUFFIXES: .c .o

.c.o:
	$(CC) $(CFLAGS) -c $<

all: $(TARGETS)

oss: oss.o
	$(CC) $(CFLAGS) -o oss oss.o

user: user.o
	$(CC) $(CFLAGS) -o user user.o

oss.o:  oss.c shared.h
user.o: user.c shared.h

clean:
	rm -f *.o $(TARGETS) log.txt
