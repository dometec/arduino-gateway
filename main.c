/**
 *
 *
 * Default serial device: /dev/ttyACM0
 *
 * author Domenico Briganti
 *
 */

#define __USE_BSD /* usleep() */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <linux/serial.h>

#define LISTEN_PORT 20118
#define ARDUINO_COMMAND_MAX_SIZE 50
#define TIMEOUT_SEC_ARDUINO_RESPONSE 2

//Domenico: per MIPS
#define bzero(p,sz) memset(p, 0, sz);

const char* serial_device_prefix = "/dev/ttyACM";
char* serial_device;
bool shutting_down = false;

int sock_fd;
int sock_udp_fd;
int tty_fd;

// Two socket address structures - One for the server itself and the other for client
struct sockaddr_in serv_addr, client_addr;
struct termios old_stdio;
char buff[ARDUINO_COMMAND_MAX_SIZE];

void termination_handler (int signum) {
    printf("Shutdown....\n\r");
    fflush(stdout);
    shutting_down = true;
    close(sock_fd);
    close(tty_fd);
    printf("Shutdown.\n\r");
    fflush(stdout);
}

void coredump_handler(int signum) {
    void *array[10];
    size_t size;


    // get void*'s for all entries on the stack
    //size = backtrace(array, 10);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", signum);
    //backtrace_symbols_fd(array, size, 2);
    exit(1);

}

void mysleep(int milliseconds) {

    nanosleep((struct timespec[]) {{
            0, milliseconds  * 1000000
        }
    }, NULL);

}

bool findArduinoSerialPort() {

    DIR *mydir = opendir("/dev");

    struct dirent *entry = NULL;
    char filename[255];
    
    while((entry = readdir(mydir))) {

        strcpy(filename, "/dev/");
        strcat(filename, entry->d_name);
	
	if (strncmp(serial_device_prefix, filename, strlen(serial_device_prefix)) == 0) {
	  printf("Arduino found in %s\n", filename);  
	  serial_device = malloc(100);
	  strcpy(serial_device, filename);  
	  closedir(mydir);
	  return true;
	}
    }

    closedir(mydir);
    if (serial_device != NULL) {
      free(serial_device);
      serial_device = NULL;
    }
    return false;

}

void openSerial() {

    struct termios tio;
    struct termios stdio;

    memset(&stdio, 0, sizeof(stdio));
    stdio.c_iflag = 0;
    stdio.c_oflag = 0;
    stdio.c_cflag = 0;
    stdio.c_lflag = 0;
    stdio.c_cc[VMIN] = 1;
    stdio.c_cc[VTIME] = 0;

    memset(&tio, 0, sizeof(tio));
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_cflag = CS8 |CREAD|CLOCAL;           // 8n1, see termios.h for more information
    tio.c_lflag = 0;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 5;

    tty_fd = open(serial_device, O_RDWR | O_NONBLOCK);      //Non blocking read
    cfsetospeed(&tio, B9600);            // 9600 baud
    cfsetispeed(&tio, B9600);            // 9600 baud

    tcsetattr(tty_fd, TCSANOW, &tio);
}

void closeSerial() {
    close(tty_fd);
}

void createServerAddressStructure() {
    // Initialize the server address struct to zero
    bzero((char *)&serv_addr, sizeof(serv_addr));

    // Fill server's address family
    serv_addr.sin_family = AF_INET;

    // Server should allow connections from any ip address
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    // 16 bit port number on which server listens
    // The function htons (host to network short) ensures that an integer is interpretted
    // correctly (whether little endian or big endian) even if client and server have different architectures
    serv_addr.sin_port = htons(LISTEN_PORT);
}

void openTCPSocket() {

    // Create socket of domain - Internet (IP) address, type - Stream based (TCP) and protocol unspecified
    // since it is only useful when underlying stack allows more than one protocol and we are choosing one.
    // 0 means choose the default protocol.
    sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    // A valid descriptor is always a positive value
    if(sock_fd < 0) {
        printf("Failed creating socket.\n\r");
        fflush(stdout);
    }

    // Attach the server socket to a port. This is required only for server since we enforce
    // that it does not select a port randomly on it's own, rather it uses the port specified
    // in serv_addr struct.
    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Failed to bind.\n\r");
        fflush(stdout);
    }

    // Server should start listening - This enables the program to halt on accept call (coming next)
    // and wait until a client connects. Also it specifies the size of pending connection requests queue
    // i.e. in this case it is 5 which means 5 clients connection requests will be held pending while
    // the server is already processing another connection request.
    listen(sock_fd, 5);
    fcntl(sock_fd, F_SETFL, O_NONBLOCK);

}

void openUDPSocket() {

    sock_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock_udp_fd < 0) {
        printf("Failed creating udp socket.\n\r");
        fflush(stdout);
    }

    if (bind(sock_udp_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Failed to bind.\n\r");
        fflush(stdout);
    }

    fcntl(sock_udp_fd, F_SETFL, O_NONBLOCK);

}

bool execArduinoCommandNoWait(char* command) {

    if (serial_device == NULL && findArduinoSerialPort())
        openSerial();

    if (serial_device == NULL) {
        printf("Arduino not found, attach it!\n\r");
        fflush(stdout);
        return NULL;
    }

    int len = strlen(command);

    printf("Executing (len %d): %s", len, command);
    fflush(stdout);

    int n = write(tty_fd, command, len);
    printf("Written %d", n);
    if (n == -1) {
        printf("Can't write to Arduino, re-search serial device.\n\r");
        fflush(stdout);
        closeSerial();
    }

    return true;
}

char* execArduinoCommand(char* command) {

    if (!execArduinoCommandNoWait(command))
        return NULL;

    mysleep(150);

    bzero(buff, ARDUINO_COMMAND_MAX_SIZE);
    int sizeRead = read(tty_fd, buff, ARDUINO_COMMAND_MAX_SIZE - 1);
    if (sizeRead <= 0) {
        printf("No response from arduino, timeout 150ms, re-search serial device.\n\r");
        fflush(stdout);
        closeSerial();

        if (findArduinoSerialPort()) {
            openSerial();

            bzero(buff, ARDUINO_COMMAND_MAX_SIZE);
            int sizeRead = read(tty_fd, buff, ARDUINO_COMMAND_MAX_SIZE - 1);
            if (sizeRead <= 0) {
                printf("No response from arduino, timeout 150ms.\n\r");
                fflush(stdout);
                return NULL;
            }

            printf("Letto (len %d): %s\n\r", sizeRead, buff);
            fflush(stdout);
            return buff;

        } else {
            printf("Arduino not found, attach it!\n\r");
            fflush(stdout);
            return NULL;
        }

    }

    printf("Letto (len %d): %s\n\r", sizeRead, buff);
    fflush(stdout);

    return buff;


}

int main(int argc, char *argv[]) {

    signal(SIGSEGV, coredump_handler);
    signal(SIGINT, termination_handler);

    if (findArduinoSerialPort())
        openSerial();
    else {
        printf("Arduino not found, attach it!\n\r");
        fflush(stdout);
    }

    createServerAddressStructure();
    openUDPSocket();
    openTCPSocket();

    while (!shutting_down) {

        //Legge UDP
        bzero(buff, ARDUINO_COMMAND_MAX_SIZE);
        ssize_t count = recvfrom(sock_udp_fd, buff, sizeof(buff), 0, NULL, NULL);
        if (count > 0)
            execArduinoCommand(buff);

        //Legge TCP
        // Server blocks on this call until a client tries to establish connection.
        // When a connection is established, it returns a 'connected socket descriptor' different
        // from the one created earlier.
        int size = sizeof(client_addr);
        int conn_desc = accept(sock_fd, NULL, NULL);
        if (conn_desc != -1 && conn_desc != EWOULDBLOCK) {
            bzero(buff, ARDUINO_COMMAND_MAX_SIZE);
            int sizeRead = read(conn_desc, buff, ARDUINO_COMMAND_MAX_SIZE - 1);
            if (sizeRead > 0) {
                char* output = execArduinoCommand(buff);
                if (output != NULL)
                    write(conn_desc, output, strlen(output));
            }
            close(conn_desc);
        }

        mysleep(10);

    }

}

