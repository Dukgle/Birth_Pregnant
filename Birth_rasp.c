#ifdef RaspberryPi

// Pthread create and join
// gcc -o [OUTFILE] [SOURCE] -lpthread
// Car - pthread + Ultrasonic + I2C(LED Screen) + Buzzer

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <stdint.h> // uint8_t definitions
#include <pthread.h>

// wiring Pi
#include <wiringPi.h>
#include <wiringSerial.h>

#include <portaudio.h>
#include <sndfile.h>

char device[] = "/dev/ttyUSB0";
int fd;
unsigned long baud = 9600;
unsigned long myTime = 0;
char notEmpty = '0';	// 1: seat, 0: not seat
char pregnant = '0';	// 1: pregnant, 0: not pregnant
int client = -1;
int speaker = 0;
int serial_port;

typedef struct
{
    SNDFILE *sndfile;
    SF_INFO sfinfo;
} paTestData;

// Forward declaration of paCallback
static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo *timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData);
                      
// Declare a static buffer for read_server
static char input[1024] = { 0 };

void playAudio(const char *filePath);

static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo *timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData)
{
    paTestData *data = (paTestData *)userData;
    sf_count_t framesRead = sf_readf_float(data->sndfile, (float *)outputBuffer, framesPerBuffer);

    if (framesRead < framesPerBuffer)
    {
        sf_seek(data->sndfile, 0, SEEK_SET);
    }

    return 0;
}

bdaddr_t bdaddr_any = {0, 0, 0, 0, 0, 0};
bdaddr_t bdaddr_local = {0, 0, 0, 0xff, 0xff, 0xff};

int _str2uuid(const char* uuid_str, uuid_t* uuid) {

    uint32_t uuid_int[4];
    char* endptr;

    if (strlen(uuid_str) == 36) {
        char buf[9] = { 0 };

        if (uuid_str[8] != '-' && uuid_str[13] != '-' &&
            uuid_str[18] != '-' && uuid_str[23] != '-') {
            return 0;
        }
        // first 8-bytes
        strncpy(buf, uuid_str, 8);
        uuid_int[0] = htonl(strtoul(buf, &endptr, 16));
        if (endptr != buf + 8) return 0;
        // second 8-bytes
        strncpy(buf, uuid_str + 9, 4);
        strncpy(buf + 4, uuid_str + 14, 4);
        uuid_int[1] = htonl(strtoul(buf, &endptr, 16));
        if (endptr != buf + 8) return 0;

        // third 8-bytes
        strncpy(buf, uuid_str + 19, 4);
        strncpy(buf + 4, uuid_str + 24, 4);
        uuid_int[2] = htonl(strtoul(buf, &endptr, 16));
        if (endptr != buf + 8) return 0;

        // fourth 8-bytes
        strncpy(buf, uuid_str + 28, 8);
        uuid_int[3] = htonl(strtoul(buf, &endptr, 16));
        if (endptr != buf + 8) return 0;

        if (uuid != NULL) sdp_uuid128_create(uuid, uuid_int);
    }
    else if (strlen(uuid_str) == 8) {
        // 32-bit reserved UUID
        uint32_t i = strtoul(uuid_str, &endptr, 16);
        if (endptr != uuid_str + 8) return 0;
        if (uuid != NULL) sdp_uuid32_create(uuid, i);
    }
    else if (strlen(uuid_str) == 4) {
        // 16-bit reserved UUID
        int i = strtol(uuid_str, &endptr, 16);
        if (endptr != uuid_str + 4) return 0;
        if (uuid != NULL) sdp_uuid16_create(uuid, i);
    }
    else {
        return 0;
    }

    return 1;

}

sdp_session_t* register_service(uint8_t rfcomm_channel) {

    const char* service_name = "Armatus Bluetooth server";
    const char* svc_dsc = "A HERMIT server that interfaces with the Armatus Android app";
    const char* service_prov = "Armatus";

    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid,
        svc_class_uuid;
    sdp_list_t* l2cap_list = 0,
        * rfcomm_list = 0,
        * root_list = 0,
        * proto_list = 0,
        * access_proto_list = 0,
        * svc_class_list = 0,
        * profile_list = 0;
    sdp_data_t* channel = 0;
    sdp_profile_desc_t profile;
    sdp_record_t record = { 0 };
    sdp_session_t* session = 0;

    // set the general service ID
    _str2uuid("00001101-0000-1000-8000-00805F9B34FB", &svc_uuid);
    sdp_set_service_id(&record, svc_uuid);

    char str[256] = "";
    sdp_uuid2strn(&svc_uuid, str, 256);
    printf("Registering UUID %s\n", str);

    // set the service class
    sdp_uuid16_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
    svc_class_list = sdp_list_append(0, &svc_class_uuid);
    sdp_set_service_classes(&record, svc_class_list);

    // set the Bluetooth profile information
    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    profile_list = sdp_list_append(0, &profile);
    sdp_set_profile_descs(&record, profile_list);

    // make the service record publicly browsable
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(&record, root_list);

    // set l2cap information
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append(0, &l2cap_uuid);
    proto_list = sdp_list_append(0, l2cap_list);

    // register the RFCOMM channel for RFCOMM sockets
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
    sdp_list_append(rfcomm_list, channel);
    sdp_list_append(proto_list, rfcomm_list);

    access_proto_list = sdp_list_append(0, proto_list);
    sdp_set_access_protos(&record, access_proto_list);

    // set the name, provider, and description
    sdp_set_info_attr(&record, service_name, service_prov, svc_dsc);

    // connect to the local SDP server, register the service record,
    // and disconnect
    session = sdp_connect(&bdaddr_any, &bdaddr_local, SDP_RETRY_IF_BUSY);
    sdp_record_register(session, &record, 0);

    // cleanup
    sdp_data_free(channel);
    sdp_list_free(l2cap_list, 0);
    sdp_list_free(rfcomm_list, 0);
    sdp_list_free(root_list, 0);
    sdp_list_free(access_proto_list, 0);
    sdp_list_free(svc_class_list, 0);
    sdp_list_free(profile_list, 0);

    return session;
}

void send_data(int value) {
    value = htonl(value);
    write(serial_port, &value, sizeof(value));
}

int init_server() {
    int port = 3, result, sock, client, bytes_read, bytes_sent;
    struct sockaddr_rc loc_addr = { 0 }, rem_addr = { 0 };
    char buffer[1024] = { 0 };
    socklen_t opt = sizeof(rem_addr);

    // local bluetooth adapter
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_bdaddr = bdaddr_any;
    loc_addr.rc_channel = (uint8_t)port;

    // register service
    sdp_session_t* session = register_service(port);
    // allocate socket
    sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    printf("socket() returned %d\n", sock);

    // bind socket to port 3 of the first available
    result = bind(sock, (struct sockaddr*)&loc_addr, sizeof(loc_addr));
    printf("bind() on channel %d returned %d\n", port, result);

    // put socket into listening mode
    result = listen(sock, 1);
    printf("listen() returned %d\n", result);
    
    delay(5000);
    playAudio("sound.mp3");
    send_data(0);

    // accept one connection
    printf("calling accept()\n");
    client = accept(sock, (struct sockaddr*)&rem_addr, &opt);
    printf("accept() returned %d\n", client);

    ba2str(&rem_addr.rc_bdaddr, buffer);
    fprintf(stderr, "accepted connection from %s\n", buffer);
    memset(buffer, 0, sizeof(buffer));


    return client;
}

/*
int read_server(int client) {
    char input[256];
    // read data from the client
    int bytes_read;
    bytes_read = read(client, input, sizeof(input));
    if (bytes_read > 0) {
        input[bytes_read] = '\0';
        
        printf("received: %s \n", input);
	
        int receivedValue = atoi(input);
        
        return receivedValue;
    } else {
        return -1;
    }
}*/

int read_server(int client) {
    int recvInt;
    
    int bytes_read = read(client, &recvInt, sizeof(int));
    
    recvInt = ntohl(recvInt);
    
    if (bytes_read == sizeof(int)) {
        printf("recv : %d\n", recvInt);
        return recvInt;
    } 
    else if (bytes_read > 0) {
       printf("recv : %d\n", recvInt);
       return recvInt;
    }
}

void playAudio(const char *filePath)
{
    PaStream *stream;
    PaError err;
    paTestData data;

    data.sndfile = sf_open(filePath, SFM_READ, &data.sfinfo);
    
    printf("Audio play");
    
    if (!data.sndfile)
    {
        fprintf(stderr, "Unable to open file '%s'\n", filePath);
        exit(1);
    }

    err = Pa_Initialize();
    if (err != paNoError)
        goto error;

    err = Pa_OpenDefaultStream(&stream, 0, data.sfinfo.channels, paFloat32,
                               data.sfinfo.samplerate, paFramesPerBufferUnspecified,
                               paCallback, &data);
    if (err != paNoError)
        goto error;

    err = Pa_StartStream(stream);
    if (err != paNoError)
        goto error;

    Pa_Sleep((unsigned long)((data.sfinfo.frames / data.sfinfo.samplerate) * 1000));

    err = Pa_StopStream(stream);
    if (err != paNoError)
        goto error;

    err = Pa_CloseStream(stream);
    if (err != paNoError)
        goto error;

    Pa_Terminate();

    sf_close(data.sndfile);

    return;

error:
    fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    Pa_Terminate();
    sf_close(data.sndfile);
    exit(1);
}

void write_server(int client, char status) {
    // send seat status to the Arduino
    char messageArr[2] = {status, '\0'};
    int bytes_sent = write(client, messageArr, sizeof(messageArr));
    if (bytes_sent > 0) {
        //printf("Sent seat status: %c\n", status);
    }
}



// Pressure sensor
void *psrThreadRoutine(void *argumentPointer) {
    pthread_t id = pthread_self();
    //setup();
    printf("%s \n", "Raspberry Startup!");
    fflush(stdout);

    if ((fd = serialOpen(device, baud)) < 0)
    {
        fprintf(stderr, "Unable to open serial device: %s\n", strerror(errno));
        exit(1);
    }

    if (wiringPiSetup() == -1)
    {
        fprintf(stdout, "Unable to start wiringPi: %s\n", strerror(errno));
        exit(1);
    }

    // while(1)
    for(int i = 0; ;  i++) {
	if (serialDataAvail(fd))
	{
	    if (client == -1 && notEmpty == '0')
	    {
		char newChar = serialGetchar(fd);
		delay(100);
		if (newChar == '1' && client == -1)
		{
		    notEmpty = '1';
		    printf("%s \n", "The Seat is not Empty...");
		}
		else if (newChar == '0' && client == -1)
		{
		    notEmpty = '0';
		    printf("%s \n", "Empty");
		}
		delay(100);
	    }
	    /*
	    else if (notEmpty == '1')
	    {
		char nextChar = serialGetchar(fd);
		delay(100);
		if (nextChar == '1'){
		    printf("%s \n", "Sit...!");
		    notEmpty == '1';
		}
		else if(nextChar != '1'){
		    printf("%s \n", "Stand Up...!");
		    notEmpty == '0';
		}
		delay(100);
	    }
	    else if (notEmpty == '-1')
	    {
		char finalChar = serialGetchar(fd);
		delay(100);
		if(finalChar == '0'){
		    printf("%s \n", "Stand Up......!");
		    notEmpty = '0';
		}
	    }*/
	}
    }
    // get return from Parent PThread -> always return
    return NULL;
}

// bluetooth
void *btThreadRoutine(void *argumentPointer) {
    pthread_t id = pthread_self();
    
    serial_port = open("/dev/ttyUSB0", O_RDWR);
    if (serial_port < 0) {
        perror("Error opening serial port");
        return NULL;
    }

    struct termios tty;
    memset(&tty, 0, sizeof tty);

    if (tcgetattr(serial_port, &tty) != 0) {
        perror("Error from tcgetattr");
        return NULL;
    }

    tty.c_cflag &= ~PARENB; // No parity
    tty.c_cflag &= ~CSTOPB; // 1 stop bit
    tty.c_cflag &= ~CSIZE;  // Clear bit mask for data bits
    tty.c_cflag |= CS8;     // 8 data bits
    tty.c_cflag &= ~CRTSCTS; // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // Enable receiver, Ignore modem control lines

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Disable software flow control

    tty.c_lflag &= 0; // No signaling characters, no echo, no canonical processing

    tty.c_oflag &= 0; // No remapping, no delays

    tty.c_cc[VMIN] = 0;  // Read doesn't block
    tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

    if (tcsetattr(serial_port, TCSANOW, &tty) != 0) {
        perror("Error from tcsetattr");
        return -1;
    }

    // while(1)
    for(int i = 0; ; i++) {
	if (notEmpty == '1')
	{
	    int client = init_server();
	
	
            if(read_server(client) == 0){
		pregnant = '0';
		playAudio("sound.mp3");
		printf("you are not pregnant\n");
		send_data(read_server(client));
            }
            else if(read_server(client) == 1){
		pregnant = '1';
		printf("you are pregnant\n");
		send_data(read_server(client));
                

            } else {
                printf("else");
            }
	    sleep(1);
	}
    }
    
    // get return from Parent PThread -> always return
    return NULL;
}

// LED
void *ledThreadRoutine(void *argumentPointer) {
    pthread_t id = pthread_self();
    serial_port = open("/dev/ttyUSB0", O_RDWR);
    if (serial_port < 0) {
        perror("Error opening serial port");
        return NULL;
    }

    struct termios tty;
    memset(&tty, 0, sizeof tty);

    if (tcgetattr(serial_port, &tty) != 0) {
        perror("Error from tcgetattr");
        return NULL;
    }

    tty.c_cflag &= ~PARENB; // No parity
    tty.c_cflag &= ~CSTOPB; // 1 stop bit
    tty.c_cflag &= ~CSIZE;  // Clear bit mask for data bits
    tty.c_cflag |= CS8;     // 8 data bits
    tty.c_cflag &= ~CRTSCTS; // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // Enable receiver, Ignore modem control lines

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Disable software flow control

    tty.c_lflag &= 0; // No signaling characters, no echo, no canonical processing

    tty.c_oflag &= 0; // No remapping, no delays

    tty.c_cc[VMIN] = 0;  // Read doesn't block
    tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

    if (tcsetattr(serial_port, TCSANOW, &tty) != 0) {
        perror("Error from tcsetattr");
        return -1;
    }

/*
    for(int i = 0; ; i++) {
	if (pregnant == '1' && notEmpty == '1') 
	{
	    printf("you are pregnant\n");
	    //send_data(read_server(client));
	    //send_data(pregnant);
	} 
	else if (pregnant == '0' && notEmpty == '1') 
	{
	    printf("you are not pregnant\n");
	    //send_data(read_server(client));
	} 
	else
	{
	    //printf("else");
	}
	delay(1000);
    }*/

    // get return from Parent PThread -> always return
    return NULL;
}

int main() {
	if(wiringPiSetup() == -1) {
		return -1;
	}

	pthread_t threadID;
	pthread_t threadID2;
	//pthread_t threadID3;

	// get TID with threadID,
	// play thread with func pointer 'threadRoutine'
	// Pressure sensor
	printf("Create first Thread!\n");
	pthread_create(&threadID, NULL, psrThreadRoutine, NULL);

	// Other threadID & func
	// Bluetooth
	printf("Create second Thread!\n");
	pthread_create(&threadID2, NULL, btThreadRoutine, NULL);

	// Three threadID & func
	// LED
	printf("Create third Thread!\n");
	//pthread_create(&threadID3, NULL, ledThreadRoutine, NULL);
    
	// wait for playing thread(having threadID)
	printf("Main Thread is now waiting for Child Thread!\n");

	pthread_join(threadID, NULL);
	pthread_join(threadID2, NULL);
	//pthread_join(threadID3, NULL);

	printf("Main Thread finish waiting Child Thread!\n");

	return 0;
}

#endif // #ifdef RaspberryPi