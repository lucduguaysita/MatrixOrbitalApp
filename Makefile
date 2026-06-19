CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
TARGET  = lcd_ipconfig
SRCS    = lcd_ipconfig.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	install -m 644 systemd/lcd-ipconfig.service /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable lcd-ipconfig.service
	@echo "Installed. Start with: systemctl start lcd-ipconfig"

uninstall:
	systemctl disable --now lcd-ipconfig.service || true
	rm -f /etc/systemd/system/lcd-ipconfig.service
	rm -f /usr/local/bin/$(TARGET)
	systemctl daemon-reload
