#include "system.h"

#include "mongoc-tests.h"

#include "debug.h"

char *TEST_RESULT;

void
run_test (const char *name,
          void (*func) (void))
{
   struct timeval begin;
   struct timeval end;
   struct timeval diff;
   struct timezone tz;
   double format;

   TEST_RESULT = "PASS";

   fprintf(stdout, "%-42s : ", name);
   fflush(stdout);
   bson_gettimeofday(&begin, &tz);
   func();
   bson_gettimeofday(&end, &tz);
   fprintf(stdout, "%s", TEST_RESULT);

   diff.tv_sec = end.tv_sec - begin.tv_sec;
   diff.tv_usec = end.tv_usec - begin.tv_usec;

   if (diff.tv_usec < 0) {
      diff.tv_sec -= 1;
      diff.tv_usec = diff.tv_usec + 1000000;
   }

   format = diff.tv_sec + (diff.tv_usec / 1000000.0);
   fprintf(stdout, " : %lf\n", format);
}