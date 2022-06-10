#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdarg.h>
#include <iostream>


#include "rpc/client.h"

#include "fmi2Functions.h"

extern "C" {
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#define MAXLINE 4096
}


using namespace std;

#ifdef _WIN32
template<typename T> T *get(HMODULE libraryHandle, const char *functionName) {
    void *fp = GetProcAddress(libraryHandle, functionName);
    return reinterpret_cast<T *>(fp);
}
#else
template<typename T> T *get(void *libraryHandle, const char *functionName) {
    void *fp = dlsym(libraryHandle, functionName);
    cout << functionName << " = " << fp << endl;
    return reinterpret_cast<T *>(fp);
}
# endif

void logger(fmi2ComponentEnvironment componentEnvironment, fmi2String instanceName, fmi2Status status, fmi2String category, fmi2String message, ...) {

    printf("[%d][%s] ", status, instanceName);

    va_list args;
    va_start(args, message);

    vprintf(message, args);

    va_end(args);

    printf("\n");
}

#define CALL(f) if ((status = f) != fmi2OK) goto out;



extern string server_ip;

extern string orthEcho(string);
extern int orthSetRunStatus(int);
extern int orthSetRunInterval(unsigned long interval, unsigned long stoptime);
extern fmi2Status orthFmi2DoStepLoop(fmi2Component c, fmi2Real currentCommunicationPoint, fmi2Real communicationStepSize, fmi2Boolean noSetFMUStatePriorToCurrentPoint);




int tcp_clientaaa(string cmd) {


    char a[10]="start\n";
    int s;
    int pid, tid;
    int datalen=0,retry=30;
    pid=getpid();
    tid=pid;//gettid();
    int n = 0;
    struct sockaddr_in addr;
    //bzero(&addr,sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(8079);
    addr.sin_addr.s_addr = inet_addr(server_ip.c_str());

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s<0) {
        printf(" socket error %s", strerror(errno));
        goto done;
    }
    n= connect(s,(struct sockaddr*)(&addr),sizeof(addr));
    if (n!=0) {
        printf(" connect error %s", strerror(errno));
        goto done;
    }
    //n = send(s,&tid,sizeof(tid),0);
    n = send(s,a,7,0);
    if (n<0) {
        printf(" connect error %s\n", strerror(errno));
        goto done;
    }
    while((n = read(s, &datalen, sizeof(datalen))) != sizeof(datalen)) {
        if(errno == EINTR) continue;
        if(errno == EWOULDBLOCK) {
            if(retry-- > 0) {
                usleep(100 * 1000);
                continue;
            }
            printf("timed out reading tid\n");
            goto done;
        }
    }
    printf("get length=%d\n",datalen);
done:
    close(s);
}


//struct _thread_param{
//    fmi2Component c;
//    fmi2Real currentCommunicationPoint;
//    fmi2Real communicationStepSize;
//    fmi2Boolean noSetFMUStatePriorToCurrentPoint;
//} thread_param;
//void *async_orth_loop(void *param){
//    struct _thread_param *p = ( struct _thread_param * ) param;
//    orthFmi2DoStepLoop(p->c, p->currentCommunicationPoint, p->communicationStepSize, p->noSetFMUStatePriorToCurrentPoint);
//    return NULL;
//}


int main(int argc, char *argv[]) {

    server_ip = "127.0.0.1";
    string cmd="start";
    tcp_clientaaa(cmd);
    exit(0);



    if ( argc !=2 ) {
        printf("\nError!  Usage:  %s serverip \n\n ", argv[0]);
        exit(-1);
    }

    server_ip = argv[1];


    fmi2CallbackFunctions functions = { logger, nullptr, nullptr, nullptr, nullptr };

    auto c = fmi2Instantiate("bb", fmi2CoSimulation, "{8c4e810f-3df3-4a00-8276-176fa3c9f003}", "", &functions, fmi2False, fmi2False);


    if (!c) {
        cout << "Failed to instantiate FMU." << endl;
        return 1;
    }


    string s="Hello Orthogonal, the RPC server is running fine ...";
    cout <<  orthEcho(s) << endl;



    fmi2Status status = fmi2OK;

    const fmi2Real stopTime = 1;
    const fmi2Real stepSize = 0.1;

    const fmi2ValueReference vr[2] = { 0, 2 };
    fmi2Real value[2] = { 0, 0 };

    fmi2Real time = 0;

    fmi2SetupExperiment(c, fmi2False, 0, 0, fmi2True, stopTime);
    fmi2EnterInitializationMode(c);
    fmi2ExitInitializationMode(c);


    ///////////////// ORG mode
    //const fmi2ValueReference vr11[1] = { 5 };
    //fmi2Real value11[1] = { 0.5 };
    //fmi2SetReal(c, vr11, 1, value11);
    //while (time <= stopTime) {
    //    fmi2GetReal(c, vr, 2, value);
    //    cout << time << ", " << value[0] << ", " << value[1] << endl;
    //    fmi2DoStep(c, time, stepSize, fmi2True);
    //    time += stepSize;
    //}


    orthSetRunStatus( 1 ); //running
    orthSetRunInterval( 100, 3000000 );    /// in us


    orthFmi2DoStepLoop(c, time, stepSize, fmi2True);    ///  run .....

    //pthread_t tid;
    //struct _thread_param p;
    //p.c = c;
    //p.currentCommunicationPoint = time;
    //p.communicationStepSize = stepSize;
    //p.noSetFMUStatePriorToCurrentPoint = fmi2True;
    //pthread_attr_t attr;
    //pthread_attr_init( &attr );
    //pthread_attr_setdetachstate(&attr,1);
    //pthread_create(&tid, NULL, &async_orth_loop, (void*)&p);

    fmi2GetReal(c, vr, 2, value);
    cout << time << ", " << value[0] << ", " << value[1] << endl;
    fmi2GetReal(c, vr, 2, value);
    cout << time << ", " << value[0] << ", " << value[1] << endl;
    fmi2GetReal(c, vr, 2, value);
    cout << time << ", " << value[0] << ", " << value[1] << endl;

    cout << "pass to final step" << endl;

    fmi2Terminate(c);

    fmi2FreeInstance(c);

    return 0;
}
