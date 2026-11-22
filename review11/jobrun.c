// Review-only (scrub11). Runs a command inside a new job object nested in the caller's (in a
// container: a non-silo job inside the server silo), so that the JVM's container detection can be
// checked where QueryInformationJobObject(NULL, ...) sees a nested job.
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: jobrun <command line>\n");
    return 2;
  }
  char cmd[32768] = "";
  for (int i = 1; i < argc; i++) {
    if (i > 1) strcat(cmd, " ");
    strcat(cmd, "\"");
    strcat(cmd, argv[i]);
    strcat(cmd, "\"");
  }
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job == NULL || !AssignProcessToJobObject(job, GetCurrentProcess())) {
    fprintf(stderr, "jobrun: cannot nest a job (%lu)\n", GetLastError());
    return 3;
  }
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "jobrun: CreateProcess failed (%lu)\n", GetLastError());
    return 4;
  }
  BOOL injob = FALSE;
  IsProcessInJob(pi.hProcess, job, &injob);
  fprintf(stderr, "jobrun: child in the nested job: %d\n", injob);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  return (int)code;
}
