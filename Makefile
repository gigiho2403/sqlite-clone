CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -g
TARGET  = db

$(TARGET): db.c
	$(CC) $(CFLAGS) -o $(TARGET) db.c

clean:
	rm -f $(TARGET)

.PHONY: clean
