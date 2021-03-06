
/* Design
 * main run in a loop, to get socket request from client
 * when rev cmd is fmu, fork new process, then using .so to load FMU and init FMU, either by fix time step
 *    , or by num step
 *
 * main func in block mode , handle request one by one
*/




#ifdef _WIN32
#include <Windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#define MAX_PATH 2048
#endif


#include <stdio.h>
#include <time.h>
#include <stdint.h>



#include <stdarg.h>
#include <time.h>
#include <list>
#include <iostream>
#include <stdexcept>

#include "rpc/server.h"

#include "remoting_tcp.h"

extern "C" {
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dlfcn.h>


#include "FMI2.h"
#include <rtai_lxrt.h>
}





using namespace std;

#define NOT_IMPLEMENTED return static_cast<int>(fmi2Error);

static list<LogMessage> s_logMessages;

static rpc::server *s_server = nullptr;

time_t s_lastActive;


int TCP_PORT=8079;
string PID_FILE="/tmp/child_pid.txt";



void logMessage(FMIInstance *instance, FMIStatus status, const char *category, const char *message) {
    s_logMessages.push_back({instance->name, status, category, message});
}

static void resetExitTimer() {
    time(&s_lastActive);
}

static void logFunctionCall(FMIInstance *instance, FMIStatus status, const char *message, ...) {

    va_list args;
    va_start(args, message);

    vprintf(message, args);

    va_end(args);

    switch (status) {
    case FMIOK:
        printf(" -> OK\n");
        break;
    case FMIWarning:
        printf(" -> Warning\n");
        break;
    case FMIDiscard:
        printf(" -> Discard\n");
        break;
    case FMIError:
        printf(" -> Error\n");
        break;
    case FMIFatal:
        printf(" -> Fatal\n");
        break;
    case FMIPending:
        printf(" -> Pending\n");
        break;
    default:
        printf(" -> Unknown status (%d)\n", status);
        break;
    }
}

static const char *lockFile = NULL;


void *checkLockFile(void *arg) {

    FILE* hLockFile = NULL;

    while (!hLockFile) {
        usleep(500000);
        hLockFile = fopen(lockFile, "w");
    }

    cout << "Lock file open. Exiting." << endl;

    s_server->stop();

    return NULL;
}


double compute_time(struct timespec start, struct timespec end) {
    double diffSec;
    diffSec = (double)(end.tv_sec-start.tv_sec)*1000000  + (double)(end.tv_nsec - start.tv_nsec)/1000;
    printf("Done 1 DoStep: diffSec %.6lf  us \n", diffSec);
    return diffSec;
}



void endRpcServer(int sig) {
    printf("Child exits by sig user1 \n");
    exit(0);
}


// http://rpclib.net/
class FMU {

  private:

    string libraryPath;

    void *libraryHandle = nullptr;

    ReturnValue createReturnValue(int status) {
        ReturnValue r = { status, s_logMessages };
        s_logMessages.clear();
        return r;
    }

    RealReturnValue createRealReturnValue(int status, const vector<double> &value) {
        RealReturnValue r = { status, s_logMessages, value };
        s_logMessages.clear();
        return r;
    }

    IntegerReturnValue createIntegerReturnValue(int status, const vector<int> &value) {
        IntegerReturnValue r = { status, s_logMessages, value };
        s_logMessages.clear();
        return r;
    }

    EventInfoReturnValue createEventInfoReturnValue(int status, const fmi2EventInfo *eventInfo) {
        EventInfoReturnValue r = {
            status,
            s_logMessages,
            eventInfo->newDiscreteStatesNeeded,
            eventInfo->terminateSimulation,
            eventInfo->nominalsOfContinuousStatesChanged,
            eventInfo->valuesOfContinuousStatesChanged,
            eventInfo->nextEventTimeDefined,
            eventInfo->nextEventTime,
        };
        s_logMessages.clear();
        return r;
    }




  public:

    // orthogonal added
    enum fmu_status { OrthRunStatusPause,OrthRunStatusRunning,OrthRunStatusStop, OrthRunStatusError };

    int runStatus=OrthRunStatusPause;
    unsigned long stopTime_us=5000000;
    unsigned long stepTime_us=100;


    rpc::server srv;

    FMIInstance *m_instance;

    FMU(const string &libraryPath) : srv(rpc::constants::DEFAULT_PORT) {

        this->libraryPath = libraryPath;

        srv.bind("orthEcho", [this](string const& s) {
            return ">>>" + s;
        });

        srv.bind("orthSetRunStatus", [this](int newStatus) {
            this->runStatus = newStatus ;
            printf("Reset new runStatus to [%d]", newStatus);
            return 0;
        });
        srv.bind("orthSetRunInterval", [this](unsigned long intts, unsigned long stopts) {  /// in us
            this->stopTime_us = stopts;
            this->stepTime_us=intts;
            printf("Reset running interval %ld/%ld.\n", this->stepTime_us, this->stopTime_us);
            return 0;
        });



        srv.bind("fmi2SetDebugLogging", [this]() {
            NOT_IMPLEMENTED
        });


        /* Creation and destruction of FMU instances and setting debug status */
        srv.bind("fmi2Instantiate", [this](string const& instanceName, int fmuType, string const& fmuGUID, string const& fmuResourceLocation, int visible, int loggingOn) {

            m_instance = FMICreateInstance(instanceName.c_str(), this->libraryPath.c_str(), logMessage, nullptr);

            if (!m_instance) {
                return createReturnValue(0);
            }

            resetExitTimer();

            fmi2Status status = FMI2Instantiate(m_instance, fmuResourceLocation.c_str(), static_cast<fmi2Type>(fmuType), fmuGUID.c_str(), visible, loggingOn);

            if (status > fmi2Warning) {
                return createReturnValue(0);
            }

            long int_value = reinterpret_cast<long>(m_instance);

            return createReturnValue(static_cast<int>(int_value));
        });

        srv.bind("fmi2FreeInstance", [this]() {
            resetExitTimer();
            FMI2FreeInstance(m_instance);
        });

        /* Enter and exit initialization mode, terminate and reset */
        srv.bind("fmi2SetupExperiment", [this](int toleranceDefined, double tolerance, double startTime, int stopTimeDefined, double stopTime) {
            resetExitTimer();
            const fmi2Status status = FMI2SetupExperiment(m_instance, toleranceDefined, tolerance, startTime, stopTimeDefined, stopTime);
            return createReturnValue(status);
        });

        srv.bind("fmi2EnterInitializationMode", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2EnterInitializationMode(m_instance);
            return createReturnValue(status);
        });

        srv.bind("fmi2ExitInitializationMode",  [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2ExitInitializationMode(m_instance);
            return createReturnValue(status);
        });

        srv.bind("fmi2Terminate", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2Terminate(m_instance);
            return createReturnValue(status);
        });

        srv.bind("fmi2Reset", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2Reset(m_instance);
            return createReturnValue(status);
        });

        /* Getting and setting variable values */
        srv.bind("fmi2GetReal", [this](const vector<unsigned int> &vr) {
            resetExitTimer();
            vector<double> value(vr.size());
            const fmi2Status status = FMI2GetReal(m_instance, vr.data(), vr.size(), value.data());
            return createRealReturnValue(status, value);
        });

        srv.bind("fmi2GetInteger", [this](const vector<unsigned int> &vr) {
            resetExitTimer();
            vector<int> value(vr.size());
            const fmi2Status status = FMI2GetInteger(m_instance, vr.data(), vr.size(), value.data());
            return createIntegerReturnValue(status, value);
        });

        srv.bind("fmi2GetBoolean", [this](const vector<unsigned int> &vr) {
            resetExitTimer();
            vector<int> value(vr.size());
            const fmi2Status status = FMI2GetBoolean(m_instance, vr.data(), vr.size(), value.data());
            return createIntegerReturnValue(status, value);
        });

        srv.bind("fmi2SetReal", [this](const vector<unsigned int> &vr, const vector<double> &value) {
            resetExitTimer();
            const fmi2Status status = FMI2SetReal(m_instance, vr.data(), vr.size(), value.data());
            return createReturnValue(status);
        });

        srv.bind("fmi2SetInteger", [this](const vector<unsigned int> &vr, const vector<int> &value) {
            resetExitTimer();
            const fmi2Status status = FMI2SetInteger(m_instance, vr.data(), vr.size(), value.data());
            return createReturnValue(status);
        });

        srv.bind("fmi2SetBoolean", [this](const vector<unsigned int> &vr, const vector<int> &value) {
            resetExitTimer();
            const fmi2Status status = FMI2SetBoolean(m_instance, vr.data(), vr.size(), value.data());
            return createReturnValue(status);
        });

        /* Getting and setting the internal FMU state */
        // fmi2GetFMUstateTYPE *m_fmi2Component c, fmi2FMUstate* FMUstate);
        // fmi2SetFMUstateTYPE *m_fmi2Component c, fmi2FMUstate  FMUstate);
        // fmi2FreeFMUstateTYPE *m_fmi2Component c, fmi2FMUstate* FMUstate);
        // fmi2SerializedFMUstateSizeTYPE *m_fmi2Component c, fmi2FMUstate  FMUstate, size_t* size);
        // fmi2SerializeFMUstateTYPE *m_fmi2Component c, fmi2FMUstate  FMUstate, fmi2Byte[], size_t size);
        // fmi2DeSerializeFMUstateTYPE *m_fmi2Component c, const fmi2Byte serializedState[], size_t size, fmi2FMUstate* FMUstate);

        srv.bind("fmi2GetDirectionalDerivative", [this](const vector<unsigned int> &vUnknown_ref, const vector<unsigned int> &vKnown_ref, const vector<double> &dvKnown) {
            resetExitTimer();
            vector<double> dvUnknown(vKnown_ref.size());
            const fmi2Status status = FMI2GetDirectionalDerivative(m_instance, vUnknown_ref.data(), vUnknown_ref.size(),
                                      vKnown_ref.data(), vKnown_ref.size(), dvKnown.data(), dvUnknown.data());
            return createRealReturnValue(status, dvUnknown);
        });

        /***************************************************
        Types for Functions for FMI2 for Model Exchange
        ****************************************************/

        /* Enter and exit the different modes */
        srv.bind("fmi2EnterEventMode", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2EnterEventMode(m_instance);
            return createReturnValue(status);
        });

        srv.bind("fmi2NewDiscreteStates", [this]() {
            resetExitTimer();
            fmi2EventInfo eventInfo = { 0 };
            const fmi2Status status = FMI2NewDiscreteStates(m_instance, &eventInfo);
            return createEventInfoReturnValue(status, &eventInfo);
        });

        srv.bind("fmi2EnterContinuousTimeMode", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2EnterContinuousTimeMode(m_instance);
            return createReturnValue(status);
        });

        srv.bind("fmi2CompletedIntegratorStep", [this](int noSetFMUStatePriorToCurrentPoint) {
            resetExitTimer();
            vector<int> value(2);
            fmi2Boolean* enterEventMode = &(value.data()[0]);
            fmi2Boolean* terminateSimulation = &(value.data()[1]);
            const fmi2Status status = FMI2CompletedIntegratorStep(m_instance, noSetFMUStatePriorToCurrentPoint, enterEventMode, terminateSimulation);
            return createIntegerReturnValue(status, value);
        });

        /* Providing independent variables and re-initialization of caching */
        srv.bind("fmi2SetTime", [this](double time) {
            resetExitTimer();
            const fmi2Status status = FMI2SetTime(m_instance, time);
            return createReturnValue(status);
        });

        srv.bind("fmi2SetContinuousStates", [this](const vector<double> &x) {
            resetExitTimer();
            const fmi2Status status = FMI2SetContinuousStates(m_instance, x.data(), x.size());
            return createReturnValue(status);
        });

        /* Evaluation of the model equations */
        srv.bind("fmi2GetDerivatives", [this](size_t nx) {
            resetExitTimer();
            vector<double> derivatives(nx);
            const fmi2Status status = FMI2GetDerivatives(m_instance, derivatives.data(), nx);
            return createRealReturnValue(status, derivatives);
        });

        srv.bind("fmi2GetEventIndicators", [this](size_t ni) {
            resetExitTimer();
            vector<double> eventIndicators(ni);
            const fmi2Status status = FMI2GetEventIndicators(m_instance, eventIndicators.data(), ni);
            return createRealReturnValue(status, eventIndicators);
        });

        srv.bind("fmi2GetContinuousStates", [this](size_t nx) {
            resetExitTimer();
            vector<double> x(nx);
            const fmi2Status status = FMI2GetContinuousStates(m_instance, x.data(), nx);
            return createRealReturnValue(status, x);
        });

        srv.bind("fmi2GetNominalsOfContinuousStates", [this](size_t nx) {
            resetExitTimer();
            vector<double> x_nominal(nx);
            const fmi2Status status = FMI2GetNominalsOfContinuousStates(m_instance, x_nominal.data(), nx);
            return createRealReturnValue(status, x_nominal);
        });

        /***************************************************
        Types for Functions for FMI2 for Co-Simulation
        ****************************************************/

        /* Simulating the slave */
        srv.bind("fmi2SetRealInputDerivatives", [this](const vector<unsigned int> &vr, const vector<int> &order, const vector<double> &value) {
            resetExitTimer();
            const fmi2Status status = FMI2SetRealInputDerivatives(m_instance, vr.data(), vr.size(), order.data(), value.data());
            return createReturnValue(status);
        });

        srv.bind("fmi2GetRealOutputDerivatives",[this](const vector<unsigned int> &vr, const vector<int> &order) {
            resetExitTimer();
            vector<double> value(vr.size());
            const fmi2Status status = FMI2GetRealOutputDerivatives(m_instance, vr.data(), vr.size(), order.data(), value.data());
            return createRealReturnValue(status, value);
        });

        srv.bind("fmi2DoStep", [this](double currentCommunicationPoint, double communicationStepSize, int noSetFMUStatePriorToCurrentPoint) {
            resetExitTimer();
            const fmi2Status status = FMI2DoStep(m_instance, currentCommunicationPoint, communicationStepSize, noSetFMUStatePriorToCurrentPoint);
            return createReturnValue(status);
        });



        ////// Orthogonal Changed to using LOOP
        srv.bind("orthtest", [this](double currentCommunicationPoint, double communicationStepSize, int noSetFMUStatePriorToCurrentPoint) {
            fmi2Status status;
            printf("Now working on pthread\n");
            return createReturnValue(status);
        });


        srv.bind("orthFmi2DoStepLoop", [this](double currentCommunicationPoint, double communicationStepSize, int noSetFMUStatePriorToCurrentPoint) {

            struct timespec current,org;
            long diffSec;
            fmi2Status status;

            unsigned long overrun;

            RT_TASK *task=0;

            int period=0 ;

            unsigned long cnt=0;

            //signal(SIGUSR1,endRpcServer);
            //signal(SIGTERM,endHandler);
            //signal(SIGALRM,endHandler);

            if(!(task=rt_task_init_schmod(nam2num("TEST"),0,0, 0,SCHED_FIFO,0x0f))) {
                printf("Can't initial the task\n");
                exit(1);
            }

            mlockall(MCL_CURRENT|MCL_FUTURE);

            period=start_rt_timer(nano2count(this->stepTime_us*1000));

            rt_make_hard_real_time();

            rt_task_make_periodic(task,rt_get_time()+period*10,period);

            clock_gettime(CLOCK_MONOTONIC, &org);

            printf( "==== start loop:  stopTime[%ld], stepTime[%ld] %d, pid=%d\n", this->stopTime_us, this->stepTime_us,  this->runStatus, getpid()) ;

            cnt = 0;

            while( ((this->runStatus==OrthRunStatusRunning) || (this->runStatus==OrthRunStatusPause))  && cnt <= this->stopTime_us ) {

                if ( this->runStatus==OrthRunStatusRunning ) {

                    //clock_gettime(CLOCK_MONOTONIC, &current);
                    //diffSec=((current.tv_sec-org.tv_sec)*1000000000 + (current.tv_nsec - org.tv_nsec))/1000000;
                    //printf("Time mesaure %ld ms = Count measure  %ld times/1ms\n", diffSec*1000, cnt);

                    struct timespec start, end;
                    clock_gettime(CLOCK_MONOTONIC, &start);

                    resetExitTimer();
                    status = FMI2DoStep(m_instance, currentCommunicationPoint, communicationStepSize, noSetFMUStatePriorToCurrentPoint);
                    clock_gettime(CLOCK_MONOTONIC, &end);

                    if ( cnt%1000000 ==0 ) {
                        compute_time(start, end);
                        printf( "==== Fixed time step/us loop: %ld/%ld\n", cnt, this->stopTime_us);
                    }

                    cnt = cnt + stepTime_us;
                }


                // 1, read IO input, setReal -> variable,
                // 2, setReal getReal

                rt_task_wait_period();

            }


            if  ( cnt>=this->stopTime_us ) {
                this->runStatus = OrthRunStatusStop ;
            }


            rt_make_soft_real_time();

            stop_rt_timer();

            rt_task_delete(task);

            return createReturnValue(status);

        });








        srv.bind("fmi2CancelStep", [this]() {
            resetExitTimer();
            const fmi2Status status = FMI2CancelStep(m_instance);
            return createReturnValue(status);
        });

        /* Inquire slave status */
        srv.bind("fmi2GetStatus", [this](int s) {
            resetExitTimer();
            vector<int> value(1);
            const fmi2Status status = FMI2GetStatus(m_instance, fmi2StatusKind(s), reinterpret_cast<fmi2Status *>(value.data()));
            return createIntegerReturnValue(status, value);
        });

        srv.bind("fmi2GetRealStatus", [this](int s) {
            resetExitTimer();
            vector<double> value(1);
            const fmi2Status status = FMI2GetRealStatus(m_instance, fmi2StatusKind(s), value.data());
            return createRealReturnValue(status, value);
        });

        srv.bind("fmi2GetIntegerStatus", [this](int s) {
            resetExitTimer();
            vector<int> value(1);
            const fmi2Status status = FMI2GetIntegerStatus(m_instance, fmi2StatusKind(s), value.data());
            return createIntegerReturnValue(status, value);
        });

        srv.bind("fmi2GetBooleanStatus", [this](int s) {
            resetExitTimer();
            vector<int> value(1);
            const fmi2Status status = FMI2GetBooleanStatus(m_instance, fmi2StatusKind(s), value.data());
            return createIntegerReturnValue(status, value);
        });

        //fmi2GetStringStatusTYPE  *m_fmi2GetStringStatus;

    }

};

FMU *fmu_p;


void set_child_pid( int pid) {
    FILE *fp = fopen(PID_FILE.c_str(), "w");
    fprintf(fp, "%d", pid );
    fclose(fp);
}

int get_child_pid() {
    int pid;

    FILE *fp = fopen (PID_FILE.c_str(), "r");
    if ( fp ==NULL) {
        return 0;
    }
    fscanf(fp, "%d", &pid);
    fclose(fp);
    return pid;
}


//////////// my application starts here ////////////////////////////////////////////////////////

void endHandler(int sig) {
//    xxxend=0;
}







int orthTest() {

    //rtaitest();

    printf ("OrthTest received... return success !\n");
    return 0;
}



int orthStart() {
    printf (" %s!\n", __FUNCTION__);
    return 0;
}
int orthStop() {
    printf (" %s!\n", __FUNCTION__);
    return 0;
}
int orthPause() {
    printf (" %s!\n", __FUNCTION__);
    return 0;
}
int orthResume() {
    printf (" %s!\n", __FUNCTION__);
    return 0;
}
int orthGetResults(int from_last_idx) {
    printf (" %s!\n", __FUNCTION__);
    return 0;
}



double divide(double a, double b) {
    return a / b;
}

struct subtractor {
    double operator()(double a, double b) {
        return a - b;
    }
};

struct multiplier {
    double multiply(double a, double b) {
        return a * b;
    }
};




typedef struct _fmu_thread_mgmt {

    //fmu_status statusiii;

    string fmu_lib_path;

} fmu_thread_mgmt;






int orth_func_load_fmu( fmu_thread_mgmt *fmu_thread_param ) {

    struct stat   buffer;

    signal(SIGUSR1,endRpcServer);

    fmu_thread_mgmt *param=(fmu_thread_mgmt*)fmu_thread_param;

    printf("Enter into thread function  [%s]\n", param->fmu_lib_path.data() );

    if (  stat (param->fmu_lib_path.data(), &buffer) == 0 && S_ISREG(buffer.st_mode)  ) {
        ;
    } else {
        cerr << "shared_library[" << param->fmu_lib_path <<"] not found, FMU thread exit... " << endl;
        return EXIT_FAILURE;
    }

    try {

        cout << "Loading... " <<  param->fmu_lib_path  << endl;

        fmu_p = new FMU( param->fmu_lib_path.data() );

        s_server = &fmu_p->srv;

        time(&s_lastActive);

        cout << "Starting RPC server" << endl;

        //constexpr size_t thread_count = 4;
        //fmu_p->srv.async_run( thread_count );

        fmu_p->srv.run();

    } catch (const std::exception& e) {
        cerr << e.what() << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}





void run_child(int sock) {

    FILE *in = fdopen(sock,"r");
    FILE *out = fdopen(sock,"w");

    char ch[256];

    setvbuf(in, NULL, _IOFBF, 1024);
    setvbuf(out, NULL, _IOLBF, 1024);


    // run only once !!!
    while ( fgets(ch,255,in) ) {

        if ( strncmp(ch,"@",1 )==0 )
            break;

        if ( strncmp(ch,"start", 5) ==0 ) {
            printf("Now to start FMU\n");
            fputs("FMU has been started\n", out);
            break;
        } else if ( strncmp(ch,"stop", 4)==0 ) {

            fmu_p->srv.stop();

            printf("Now stop FMU\n");
            fputs("FMU has been Stopped\n", out);

            delete fmu_p;

            break;

        } else if ( strncmp(ch,"pause", 5)==0 ) {
            printf("Now pause FMU\n");
            fputs("FMU has been paused\n", out);
        } else if ( strncmp(ch,"load", 4)==0 ) {

            close(sock);

            int tmppid=get_child_pid();
            printf("In child PROCRESS, %d, old pid=%d\n", getpid(), tmppid);


            if (tmppid>0) {
                printf("child itry killing  %d\n", tmppid);

                kill(tmppid, SIGINT);  // to kill child
                kill(tmppid, SIGUSR1);  // to kill child


                wait(NULL);
            }

            usleep( 500000);


            set_child_pid(getpid());

            printf("child is listening ... %d\n", get_child_pid());


            int err;
            void *rst, *tmpptr;
            pthread_t tid;

            fmu_thread_mgmt ftm;

            // debug temp path
            ftm.fmu_lib_path = "/tmp/fmu.so";

            tmpptr = dlopen(ftm.fmu_lib_path.c_str(), RTLD_LAZY);
            if ( tmpptr == NULL || dlsym( tmpptr, "fmi2Instantiate") == NULL ) {
                if ( tmpptr )
                    dlclose( tmpptr );
                fputs("Module error\n", out);
                break ;
            }
            dlclose( tmpptr );


            if ( orth_func_load_fmu (&ftm) != EXIT_SUCCESS ) {
                printf("Can't init  and load fmu \n");
                fputs("Can't init  and load fmu \n", out);
                break;
            }

            ////should not be thread , when loading FMU it is in main thread ok.
            //////err = pthread_create(&tid, NULL, &orth_func_load_fmu, (void*)&ftm);
            //if (err != 0) {
            //    printf("Can't create thread :[%s]", strerror(err));
            //    fputs("Thread error\n", out);
            //    break ;
            // }
            //pthread_join(tid,(void**)&rst);
            //if ( *((int*)&rst)  == 0 ) {
            //    fputs("FMU is ready to run ...\n", out);
            //    break ;
            //}
            //else {
            //    fputs("FMU failed to run ...\n", out);
            //    break ;
            //}


        } else {
            fputs("Invalie option: [start|stop|run|load]\n", out);
            break ;
        }


        break;
    }

    fclose(out);
}




int fork2() {
    pid_t pid;
    int rc;
    int status;

    if (!(pid = fork())) {
        switch (fork()) {
        case 0:
            return 0;
        case -1:
            _exit(errno);    /* assumes all errnos are <256 */
        default:
            _exit(0);
        }
    }

    if (pid < 0 || waitpid(pid,&status,0) < 0)
        return -1;

    if (WIFEXITED(status))
        if (WEXITSTATUS(status) == 0)
            return 1;
        else
            errno = WEXITSTATUS(status);
    else
        errno = EINTR;  /* well, sort of :-) */

    return -1;
}


void process() {





    unlink(PID_FILE.c_str());

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int flag = 1;
    int rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    if (rc < 0) {
        printf( "create socket error\n");
        exit(-1);
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    rc = bind(sock, (struct sockaddr *) &addr, addrlen);
    if (rc < 0) {
        printf("bind socket error\n");
        exit(-1);
    }

    rc = listen(sock, 5);
    if (rc < 0) {
        printf( "listen socket error\n");
        exit(-1);
    }



    // working as daemon to load/reload different FMUs

    for (;;) {

        rc = accept(sock, (struct sockaddr *) &addr, &addrlen);

        if (rc >= 0) {

            switch (fork2()) {
            case 0:
                close(sock);
                run_child(rc);
                _exit(0);
            case -1:
                printf( "fork2");
                close(rc);
                break;
            default:
                close(rc);
            }
        }
    }
}


void start_default_simulate() {

    int err;
    void *rst, *tmpptr;
    pthread_t tid;

    fmu_thread_mgmt ftm;

    // debug temp path
    ftm.fmu_lib_path = "/tmp/fmu.so";

    tmpptr = dlopen(ftm.fmu_lib_path.c_str(), RTLD_LAZY);
    if ( tmpptr == NULL || dlsym( tmpptr, "fmi2Instantiate") == NULL ) {
        if ( tmpptr )
            dlclose( tmpptr );
    }
    dlclose( tmpptr );

    if ( orth_func_load_fmu (&ftm) != EXIT_SUCCESS ) {
        printf("Can't init  and load fmu \n");
    }

}


int main( int argc, char **argv) {

    //networking waiting for   commands
    //process();


    ////  starting server .......... need a signal ..
    start_default_simulate();


    return EXIT_SUCCESS;

}
