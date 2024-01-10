#define _POSIX_C_SOURCE 200112L 
#define M 1000000L
#define N_DOCTORES 5
#define N_PACIENTES 1000
#define N_MEDICINES 10
#define MAX_WAIT 10
#define T_PERIODO 5

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h> 

// Estructuras
enum tipopet{
	CONSULTA,
	CARTILLA
};
struct pet{
	enum tipopet tipo;
	int dni;
	int emergencia;
};

struct argsConsulta{
	int i;
	struct pet peticion;
};

struct PatientData{
	int dni;
	char medicine[30];
	int maxtime;
	struct timespec timestamp;
};

// Variables globales
struct pet enConsulta[N_DOCTORES];
struct PatientData patientTable[N_PACIENTES];

//Mutex
pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t free_doc = PTHREAD_COND_INITIALIZER;


//Funciones de arranque
void *h_consulta(void *p);
void handler();
void crea_peticiones();
void respuestas_servidor();



int main(int argc, char **argv)	{
	//DECLARACIONES
	//Colas
	char *cola_pet = "/cola_peticiones";
	char *buffer_pet = "/buffer_peticiones";
	struct mq_attr attr, estado_cola;
	mqd_t colapeticiones, bufferpeticiones;
	
	//Señales
	struct sigaction accion;
	sigset_t mascara;
	
	//Timers
	struct timespec tciclo;
	struct itimerspec itciclo;
	timer_t timer;
	
	//Variables
	pid_t *p = malloc(N_DOCTORES*sizeof(pid_t));
	struct argsConsulta *args_consulta;
	int i;
	int prior;
	int fin=0;
	int res;
	struct pet peticion;
	
	//Hilos simulación
	pthread_t creapeticiones, respuestaservidor;
	

	// Colas
		attr.mq_maxmsg = MAX_WAIT;
		attr.mq_msgsize= sizeof(struct pet);
		mq_unlink(cola_pet); mq_unlink(buffer_pet);
		colapeticiones   = mq_open(cola_pet, O_CREAT|O_RDONLY|O_NONBLOCK, S_IRWXU, &attr);
		bufferpeticiones = mq_open(buffer_pet, O_CREAT|O_RDWR|O_NONBLOCK, S_IRWXU, &attr);
		
	// Máscara a SIGALRM para función
		accion.sa_sigaction = handler;
		accion.sa_flags     = SA_SIGINFO;
		sigemptyset(&accion.sa_mask);
		sigaction(SIGALRM, &accion, NULL);
		sigemptyset(&mascara);
		sigaddset(&mascara, SIGALRM);
		pthread_sigmask(SIG_BLOCK, &mascara, NULL);
		
	// Timers
		tciclo.tv_nsec=0; tciclo.tv_sec=T_PERIODO;
		itciclo.it_interval=tciclo; itciclo.it_value=tciclo;
	timer_create(CLOCK_REALTIME, NULL, &timer);
	timer_settime(timer, 0, &itciclo, NULL);
	
	printf("Programa arrancado\n");

	// Creamos los hilos que simulan ser pacientes del hospital y los doctores que prescriben medicamentos
	pthread_create(&creapeticiones, NULL, crea_peticiones, NULL);
	pthread_create(&respuestaservidor, NULL, respuestas_servidor, NULL);
	
	// Bucle: mientras no haya un error en la lectura ni queden demasiados mensajes pendientes
	do{
		//Leer peticiones. Si el buffer de cola está vacío (no hay solicitudes pendientes) podemos quedarnos esperando una petición.
		//En cambio, si hay peticiones encoladas, solo lo intentamos una vez
		mq_getattr(bufferpeticiones, &estado_cola);
		
		printf("\n --------------------------------------------- \n");
		    printf("En el buffer hay %i peticiones\n", estado_cola.mq_curmsgs);
		    
			if(estado_cola.mq_curmsgs == MAX_WAIT) fin=1; //Condición de fin
		if(estado_cola.mq_curmsgs == 0){
			do{
				res = mq_receive(colapeticiones, (char *)&peticion, sizeof(peticion), NULL);
			}while(res==-1);
		}
		else
			res = mq_receive(colapeticiones, (char *)&peticion, sizeof(peticion), NULL);
			
			printf("main: Se ha recibido la peticion del DNI %i. Tipo:%d. Urgencia: %i\n", peticion.dni, peticion.tipo, peticion.emergencia);
			
		//Si recibimos peticion y no es urgente, la mandamos por el buffer
		if(peticion.emergencia==0 && res!=-1)
			mq_send(bufferpeticiones, (char *)&peticion, sizeof(peticion), 0);
		else if(peticion.emergencia==1 && peticion.tipo == CONSULTA && res!=-1){
			//Comprobamos si hay un doctor libre que no esté atendiendo una emergencia
			for(i=0; i<N_DOCTORES; i++){
				if(enConsulta[i].emergencia == 0){ //En caso afirmativo, extraemos la información y la pasamos a la cola buffer con mayor prioridad
					pthread_cancel(&p[i]);
					mq_send(bufferpeticiones, &enConsulta[i], sizeof(peticion), 1);
					printf("La emergencia sera tratada por el doctor %i\n", i);
					break;		
				}
			}
			//Enviamos la petición urgente por la cola con prioridad 5. Será atendida inmediatamente
			mq_send(bufferpeticiones, (char *)&peticion, sizeof(peticion), 5);
		}
		
		
		//Una vez leídas las peticiones nuevas, comprobamos si podemos lanzar una nueva consulta
		res = mq_receive(bufferpeticiones, (char *)&peticion, sizeof(peticion), &prior);
		// Tipo de petición consulta
		if(peticion.tipo == CONSULTA){
			for(i=0; i<N_DOCTORES; i++){
				if(enConsulta[i].dni == 0){ //Si no hay nadie en la consulta lanzamos el hilo y actualizamos datos globales
					args_consulta->i=i;
					args_consulta->peticion = peticion;
					pthread_create(&p[i], NULL, h_consulta, (void *)args_consulta);
					enConsulta[i] = peticion;
					printf("Entra en consulta %i\n", i);
				break;
				}
				else if (i==4){ //Si no quedan consultas libres mandamos el mensaje de nuevo por el buffer
					mq_send(bufferpeticiones, (char *)&peticion, sizeof(peticion), &prior);
					printf("Todos los doctores estan ocupados y atendiendo una emergencia en este momento\n");
				}
				printf("Doctor %i atendiendo al paciente %i\n", i, enConsulta[i].dni);
			}
		}
		
		
		
		else if(peticion.tipo == CARTILLA){
		
		printf("\n --------------------------------------------- \n");
			printf("PRESCRIPCIONES PARA DNI %i\n", peticion.dni);
		printf(" --------------------------------------------- \n");
			pthread_mutex_lock(&mut);
			for(i=0; i<N_PACIENTES; i++){ //Buscamos todas las medicinas prescritas al paciente
				if(patientTable[i].dni == peticion.dni){
					printf("Medicina: %s\n", patientTable[i].medicine);
					printf("Expiracion: %d\n", patientTable[i].maxtime);
				}
			}			
		printf("\n --------------------------------------------- \n");
			pthread_mutex_unlock(&mut);
		}		
					
	}while(fin != 1);
	
		
}



void *h_consulta(void *p){
	struct argsConsulta *pointer = p;
	int i=pointer->i;
	struct pet peticion=pointer->peticion;
	
	int j;

	char *colaenvio = "\coladni";
	char *colarecepcion = "\colarecepcionmedicina";
	mqd_t cola_e_dni, cola_r_medicina;
	
	struct timespec sleeper = {2, 0};
	
	// Apertura de colas
	cola_e_dni = mq_open(colaenvio, O_WRONLY, 0, NULL);
	cola_r_medicina = mq_open(colarecepcion, O_RDONLY, 0, NULL);
	
	printf("hilo: Soy el hilo %i atendiendo a %i\n", i, peticion.dni);
	//Envia el DNI y duerme 2 segundos para simular la espera
	mq_send(cola_e_dni, peticion.dni, sizeof(int), 0);	
	nanosleep(&sleeper, NULL);
	printf("hilo: He enviado el dni al servidor\n");
	pthread_mutex_lock(&mut);
	//Busca un hueco vacío en la tabla
	for(j=0 ; j<N_PACIENTES; j++)
		if (patientTable[j].dni == 0)
			break;
	
	//Guarda la prescripción médica en la tabla global
	mq_receive(cola_r_medicina, (char *)&patientTable[j], sizeof(struct PatientData), NULL);
	
	printf("El paciente %i ha salido de consulta. Se le ha recetado %s durante %i dias\n", patientTable[j].dni, patientTable[j].medicine, patientTable[j].maxtime);
	//Indica que ha terminado la consulta en los datos globales
	enConsulta[i].dni = 0;
	pthread_mutex_unlock(&mut);
	
}




void handler(){
	double elapsedTime;
	int i;
	
	struct timespec currentTime;
	clock_gettime(CLOCK_REALTIME, &currentTime);
	
	pthread_mutex_lock(&mut);
	
	printf("\nBorrando recetas expiradas...\n");
	for(i=0; i<N_PACIENTES; i++){
		if(patientTable[i].dni != 0){ //Comprobamos registro válido
			elapsedTime = currentTime.tv_sec - patientTable[i].timestamp.tv_sec;
			if (elapsedTime > patientTable[i].maxtime){
				printf("RECETA %s DEL PACIENTE %i EXPIRADA\n", patientTable[i].medicine, patientTable[i].dni);
				patientTable[i].dni = 0;
			}
		}
	}
	
	pthread_mutex_unlock(&mut);
}


void crea_peticiones(){
	//Creamos los DNIs aleatorios
	int dni[10]={11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888, 99999999, 12345678};
	
	struct pet peticiones;
	struct timespec delay = {0, 50*100*M};
	int cont=1;
	
	//Abrimos cola por donde se mandarán las peticiones
	char *cola_pet = "/cola_peticiones";
	mqd_t colapeticiones;
	colapeticiones = mq_open(cola_pet, O_WRONLY, 0, NULL);
	
	printf("Hilo de crear peticiones arrancado\n");
	
	while(1){
		nanosleep(&delay, NULL); //Delay de 50 ms
		peticiones.dni = dni[cont%10];
		if(cont%5==0)
			peticiones.emergencia = 1;
		if(cont%4==0)
			peticiones.tipo = CARTILLA;
		else
			peticiones.tipo = CONSULTA;
			
	   	printf("Se crea la peticion %i del usuario %i\n", cont, peticiones.dni);
		
		mq_send(colapeticiones, (char *)&peticiones, sizeof(peticiones), NULL);
		cont++;		
	}	
	
}

	

void respuestas_servidor(){
	//Colas
	char *cola_r_dni = "\coladni";
	char *cola_e_medicina= "\colarecepcionmedicina";
	struct mq_attr attr;
	mqd_t colaservidor, coladni;	
	
	//Variables
	struct PatientData prescripcion;
	int cont=0;
	char medicines[10]={"Paracetamol", "Ibuprofeno", "Aspirina", "Omeprazol", "Amoxicilina", "Loratadina", "Aspirina", "Paracetamol", "Adrenalina", "Ibuprofeno"};
	int maxtime[10]={10, 14, 3, 3, 5, 14, 7, 14, 10, 5};
	int dni;


		
	//Abrimos las colas
	mq_unlink(cola_r_dni); mq_unlink(cola_e_medicina);
		attr.mq_maxmsg = N_DOCTORES;
		attr.mq_msgsize= sizeof(struct PatientData);
	colaservidor = mq_open(cola_e_medicina, O_CREAT|O_WRONLY, S_IRWXU, &attr);
	
		attr.mq_msgsize	= sizeof(int);
	coladni = mq_open(cola_r_dni, O_CREAT|O_RDONLY, S_IRWXU, &attr);
	
	
	printf("Hilo de doctores arrancado\n");
	
	while(1){
		mq_receive(coladni, (char *)&dni, sizeof(int), NULL);
		printf("doctor: Estoy pensando que recetar a %i\n", dni);
		pthread_mutex_lock(&mut);
		prescripcion.dni = dni;
		prescripcion.maxtime = maxtime[cont%10];
		strcpy(prescripcion.medicine, medicines[cont%10]);
		clock_gettime(CLOCK_REALTIME, &prescripcion.timestamp);
		
		mq_send(colaservidor, (char *)&prescripcion, sizeof(prescripcion), NULL);
		printf("servidor: Se ha mandado la prescripcion a %i\n", dni);
		
		cont++;
		pthread_mutex_unlock(&mut);
	}
	
}





#/** PhEDIT attribute block
#-11:16777215
#0:5139:default:-3:-3:0
#5139:5150:default:-3:-3:4
#5150:6636:default:-3:-3:0
#6636:6704:default:-3:-3:4
#6704:10390:default:-3:-3:0
#**  PhEDIT attribute block ends (-0000222)**/
