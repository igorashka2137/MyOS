CC      := gcc
LD      := ld
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -fno-builtin -mno-red-zone -O2 -Wall -Wextra
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

all: kernel.elf

boot.o: boot.S
	$(CC) -m32 -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: boot.o kernel.o linker.ld
	$(LD) $(LDFLAGS) boot.o kernel.o -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f *.o kernel.elf

.PHONY: all run clean

run-serial: kernel.elf
	qemu-system-i386 -kernel kernel.elf -serial stdio

.PHONY: run-serial
