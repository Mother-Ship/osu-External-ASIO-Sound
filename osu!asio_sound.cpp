#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <winuser.h>
#include <TlHelp32.h>
#include "fmod/inc/fmod.h"
#include "fmod/inc/fmod_errors.h"
#include <unordered_map>
#include "sharepool.h"

using namespace std;
FMOD_SYSTEM *fmodSystem;

struct sharepool *MyPool;
bool Work = 0;
FMOD_SOUND *KeySound;

int bufferSize = 128, driverId = 0, sampleRate = 48000;

int read(FILE *fp){
	int x=0,f=1;char ch=fgetc(fp);
	while(ch<'0'||ch>'9'){if(ch=='-')f=-1;ch=fgetc(fp);}
	while(ch>='0'&&ch<='9'){x=x*10+ch-'0';ch=fgetc(fp);}
	return x*f;
}

void init(bool forceWrite = false){
	FILE *fp;
	if(forceWrite || !(fp = fopen("config.ini", "r"))){
		fp = fopen("config.ini", "w");
		fprintf(fp, "[Config]\n");
		fprintf(fp, "Buffer Size = %d\n", bufferSize);
		fprintf(fp, "ASIO Driver ID = %d\n", driverId);
		fprintf(fp, "Sample Rate = %d\n", sampleRate);
		fclose(fp);
	}else{
		bufferSize = read(fp);
		driverId = read(fp);
		sampleRate = read(fp);
		fclose(fp);
	}
}

double CPUclock() {
	LARGE_INTEGER nFreq;
	LARGE_INTEGER t1;
	double dt;
	QueryPerformanceFrequency(&nFreq);
	QueryPerformanceCounter(&t1);
	dt = (t1.QuadPart) / (double)nFreq.QuadPart;
	return(dt * 1000);
}
void mainloop() {
	static unordered_map<HSAMPLE,FMOD_SOUND*> sample_maping;
	static unordered_map<HCHANNEL,FMOD_CHANNEL*> channel_maping;
	while(Work) {
		while(MyPool->Load.head != MyPool -> Load.tail) {
			//printf("[Load] %d -> %d\n",MyPool->Load.head,MyPool->Load.tail);
			HSAMPLE hSample = MyPool->Load.pool[MyPool->Load.head];
			MyPool->Load.head = (MyPool->Load.head+1) % LoadPoolSize;
			char name[256];
			int i;
			for(i = 0; MyPool->Load.pool[MyPool->Load.head] != 0; MyPool->Load.head = (MyPool->Load.head+1) % LoadPoolSize, i++) name[i] = MyPool->Load.pool[MyPool->Load.head];
			name[i] = 0;
			MyPool->Load.head = (MyPool->Load.head + 1) % LoadPoolSize;
			printf("Load Name: %s\nhSample: %lu\n", name, hSample);
			FMOD_RESULT err;
			if (FMOD_OK == (err = FMOD_System_CreateSound(fmodSystem, name, FMOD_LOOP_OFF | FMOD_NONBLOCKING | FMOD_LOWMEM | FMOD_MPEGSEARCH | FMOD_CREATESAMPLE | FMOD_IGNORETAGS, 0, &KeySound))) {
				printf("[FMOD] Loaded Sample (%s)\n", name);
				sample_maping.insert(pair<HSAMPLE, FMOD_SOUND*>(hSample, KeySound));
			} else printf("[FMOD] %s\n", FMOD_ErrorString(err));
			FMOD_System_Update(fmodSystem);
		}
		while(MyPool->Play.head != MyPool->Play.tail) {
			if(DETAILOUTPUT)printf("[Play] %d -> %d\n", MyPool->Play.head, MyPool->Play.tail);
			double Time = MyPool->Play.pool[MyPool->Play.head].Time;
			HSAMPLE hSample = MyPool->Play.pool[MyPool->Play.head].hSample;
			HCHANNEL Ch = MyPool->Play.pool[MyPool->Play.head].Ch;
			MyPool->Play.head = (MyPool->Play.head + 1) % PlayPoolSize;
			auto iter = sample_maping.find(hSample);
			if(iter != sample_maping.end()) {
				FMOD_CHANNEL *FCh;
				FMOD_System_PlaySound(fmodSystem, iter->second, NULL, false, &FCh);
				FMOD_Channel_SetVolume(FCh ,0.5f);
				channel_maping.insert(pair<HCHANNEL, FMOD_CHANNEL*>(Ch, FCh));
				if(DETAILOUTPUT) {
					int x;
					FMOD_Channel_GetIndex(FCh, &x);
					printf("Index: %d\n", x);
					printf("Play\nLatency: %.4lfms\nhSample: %lu\n", CPUclock()-Time, hSample);
				}
				FMOD_System_Update(fmodSystem);
			}
		}
		while(MyPool->Stop.head != MyPool->Stop.tail) {
			if(DETAILOUTPUT)printf("[Stop] %d -> %d\n", MyPool->Stop.head, MyPool->Stop.tail);
			HSAMPLE hChannel = MyPool->Stop.pool[MyPool->Stop.head];
			MyPool->Stop.head = (MyPool->Stop.head+1)%StopPoolSize;
			auto iter = channel_maping.find(hChannel);
			if(iter != channel_maping.end()) {
				FMOD_Channel_Stop(iter->second);
				if(DETAILOUTPUT)printf("Stop\nhChannel: %lu\n", hChannel);
				FMOD_System_Update(fmodSystem);
			}
		}
		//Sleep(1);
	}
}
bool UpPrivilege() {
	HANDLE hToken;
	TOKEN_PRIVILEGES tkp;
	bool result = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
	if(!result) return result;
	result = LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tkp.Privileges[0].Luid);
	if(!result) return result;
	tkp.PrivilegeCount = 1;
	tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	result = AdjustTokenPrivileges(hToken, FALSE, &tkp, sizeof(TOKEN_PRIVILEGES), (PTOKEN_PRIVILEGES)NULL, (PDWORD)NULL);
	return result;
}
HMODULE DllInject(HANDLE hProcess, const char *dllname) {
	unsigned long  (__stdcall *faddr)(void*);
	size_t abc;
	HMODULE hdll;
	HANDLE ht;
	LPVOID paddr;
	unsigned long exitcode;
	int dllnamelen;
	hdll = GetModuleHandleA("kernel32.dll");
	if(hdll == 0) return 0;
	faddr = (unsigned long (__stdcall *)(void*))GetProcAddress(hdll, "LoadLibraryA");
	if(faddr == 0) return 0;
	dllnamelen = strlen(dllname) + 1;
	paddr = VirtualAllocEx(hProcess, NULL, dllnamelen, MEM_COMMIT, PAGE_READWRITE);
	if(paddr == 0) return 0;
	WriteProcessMemory(hProcess, paddr, (void*)dllname, strlen(dllname) + 1, (SIZE_T*) &abc);
	ht = CreateRemoteThread(hProcess, NULL, 0, faddr, paddr, 0, NULL);
	if(ht == 0) {
		VirtualFreeEx(hProcess, paddr, dllnamelen, MEM_DECOMMIT);
		return 0;
	}
	WaitForSingleObject(ht, INFINITE);
	GetExitCodeThread(ht, &exitcode);
	CloseHandle(ht);
	VirtualFreeEx(hProcess, paddr, dllnamelen, MEM_DECOMMIT);
	return (HMODULE)exitcode;
}
DWORD getPID(LPCSTR ProcessName) {
	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	// Take a snapshot of all processes in the system.
	hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hProcessSnap == INVALID_HANDLE_VALUE)return 0;
	pe32.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(hProcessSnap, &pe32)) {
		CloseHandle(hProcessSnap);          // clean the snapshot object
		return 0;
	}
	DWORD dwPid = 0;
	do {
		if(!strcmp(ProcessName, pe32.szExeFile)) {
			dwPid = pe32.th32ProcessID;
			break;
		}
	} while(Process32Next(hProcessSnap, &pe32));
	CloseHandle(hProcessSnap);
	return dwPid;
}
char osuExename[256] = "osu!.exe";
int main(int argc, char* argv[]) {
    init();
	printf("FMOD Studio Low Level API (C) Firelight Technologies Pty Ltd.\n");
	
#ifdef __EXESELECT
	printf("Input the osu's executable name with extension name (exp: osu!.exe): ");
	char tmpc[256] = "";
	int tmplenth = 0, tmpst = 0;
	fgets(tmpc, 256, stdin);
	tmplenth = strlen(tmpc);
	while(tmplenth && (tmpc[tmplenth-1] == ' ' || tmpc[tmplenth-1] == '\n'|| tmpc[tmplenth-1] == '\r'))tmplenth--;
	tmpc[tmplenth] = 0;
	while(tmpst < tmplenth && (tmpc[tmpst] == ' ' || tmpc[tmpst] == '\n'|| tmpc[tmplenth-1] == '\r'))tmpst++;
	if(tmplenth != tmpst)strcpy(osuExename, tmpc + tmpst);
	puts("");
	printf("Executable name is : \"%s\"\n", osuExename);
#endif

	FMOD_RESULT initRet = FMOD_System_Create(&fmodSystem);
	if (initRet != FMOD_OK) {
		printf("Create FMOD System Failed: %s\n", FMOD_ErrorString((FMOD_RESULT)initRet));
		system("pause");
		return 0;
	}
	HANDLE hMapFile;
	LPVOID lpBase;
	hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct sharepool), "ShareMemoryForOsuASIO4Play");
	lpBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if(hMapFile&&lpBase) puts("ShareMemory Success.");
	MyPool = (struct sharepool*)lpBase;
	memset(MyPool,0,sizeof(struct sharepool));
	printf("Pool Size: %dKB\n\n", sizeof(struct sharepool)/1024);

    FMOD_System_SetDSPBufferSize(fmodSystem, bufferSize, 2);
	FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_ASIO);
	FMOD_System_SetDriver(fmodSystem, driverId);
	FMOD_System_SetSoftwareFormat(fmodSystem, sampleRate, FMOD_SPEAKERMODE_DEFAULT, FMOD_MAX_CHANNEL_WIDTH);
	initRet = FMOD_System_Init(fmodSystem, 32, FMOD_INIT_NORMAL, 0);
	if (initRet != FMOD_OK) {
		printf("FMOD System Initialize Failed: %s\n", FMOD_ErrorString((FMOD_RESULT)initRet));
		system("pause");
		return 0;
	}
	Sleep(1000);
	unsigned bufLen;
	int bufNum;
	char name[256];
	int speakerChannels;
	FMOD_System_GetDSPBufferSize(fmodSystem, &bufLen, &bufNum);
	FMOD_System_GetDriverInfo(fmodSystem, driverId, name, 255, 0, 0, 0, &speakerChannels);
	printf("FMOD System Initialize Finished.\n");
	printf("[FMOD] Device Name: %s\n", name);
	printf("[FMOD] Device Sample Rate: %d\n", sampleRate);
	printf("[FMOD] Device Channels: %d\n", speakerChannels);
	printf("[FMOD] DSP buffer size: %d * %d\n", bufLen, bufNum);
	printf("[FMOD] Latency: %.10lfms\n", bufLen * bufNum * 1000.0 / sampleRate);

	UpPrivilege();
	DWORD PID = 0;
	while(!PID) {
		PID = getPID(osuExename);
		Sleep(50);
	}
	Sleep(1000);
	
	HANDLE hProcess;
	if(!(hProcess = OpenProcess(PROCESS_ALL_ACCESS, 0, PID))) {
		puts("Opening Failed");
		system("pause");
		return 0;
	}
	{
		char ch0[512];
		strcpy(ch0, _pgmptr);
		{
			char* p = ch0;
			while(strchr(p, '\\')) {
				p = strchr(p, '\\');
				p++;
			}
			*p = '\0';
		}
		strcat(ch0, "osu!asio_sound");
		DllInject(hProcess, ch0);
	}
	Work = 1;
	HANDLE h1 = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)mainloop, 0, 0, 0);
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetThreadPriority(h1, HIGH_PRIORITY_CLASS);
	CloseHandle(h1);
	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	Work = 0;
	if (initRet == FMOD_OK && fmodSystem != nullptr) {
		FMOD_System_Release(fmodSystem);
		printf("FMOD System released.\n");
	}
	UnmapViewOfFile(lpBase);
	CloseHandle(hMapFile);
	return 0;
}
