section .text

; Store sudo password in ~/.lia/sudo_password

; Ensure directory exists
mov rdi, ~/.lia
mov rax, 0x20
syscall

; Create file if not exists
mov rdi, ~/.lia/sudo_password
mov rax, 0x40
syscall

; Write password
mov rdi, ~/.lia/sudo_password
mov rsi, lia
mov rdx, 4
mov rax, 0x21
syscall

; Set permissions
mov rdi, ~/.lia/sudo_password
mov rsi, 0o700
mov rdx, 0x40
syscall

ret