CC      := gcc
LD      := ld
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -fno-builtin -mno-red-zone -O2 -Wall -Wextra \
           -march=i686 -mno-mmx -mno-sse -mno-sse2 -mno-80387
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

all: kernel.elf

boot.o: boot.S Makefile
	$(CC) -m32 -c $< -o $@

kernel.o: kernel.c Makefile
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: boot.o kernel.o linker.ld
	$(LD) $(LDFLAGS) boot.o kernel.o -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f *.o kernel.elf qemu.log

.PHONY: all run clean

run-serial: kernel.elf
	qemu-system-i386 -kernel kernel.elf -serial stdio

.PHONY: run-serial

run-sound: kernel.elf
	qemu-system-i386 -kernel kernel.elf -serial stdio -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

.PHONY: run-sound

run-debug: kernel.elf
	rm -f qemu.log
	qemu-system-i386 -kernel kernel.elf -no-reboot -d int,cpu_reset -D qemu.log
	@echo "---- last lines of qemu.log ----"
	@tail -n 40 qemu.log

.PHONY: run-debug
