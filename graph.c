/* graph.c
 *
 * Copyright (C) 2014 MongoDB, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int
main (int argc,
      char *argv[])
{
   const char *sample;
   FILE *gnuplot;

   if (argc != 2 ||
       (0 == strcmp (argv [1], "-h")) ||
       (0 == strcmp (argv [1], "--help"))) {
      fprintf (stderr, "usage: graph sample-name\n\n");
      return EXIT_FAILURE;
   }

   sample = argv [1];
   gnuplot = popen ("gnuplot -persist", "w");

   if (gnuplot == NULL) {
      fprintf (stderr, "Failed to execute gnuplot!\n");
      return EXIT_FAILURE;
   }


   fprintf (gnuplot, "set term png size 1024,768\n");
   fprintf (gnuplot, "set out 'samples/%s/time.png'\n", sample);
   fprintf (gnuplot, "set key top left\n");
   fprintf (gnuplot, "set xlabel 'Thread Count'\n");
   fprintf (gnuplot, "set ylabel 'Time (in Seconds)'\n");
   fprintf (gnuplot, "set title 'Execution Time (1000000 iterations per thread)'\n");
   fprintf (gnuplot, "set style function linespoints\n");
   fprintf (gnuplot, "set style line 1 lw 4 lc rgb '#990042' ps 2 pt 6 pi 5\n");
   fprintf (gnuplot, "set style line 2 lw 3 lc rgb '#31f120' ps 2 pt 12 pi 3\n");
   fprintf (gnuplot, "set style line 3 lw 3 lc rgb '#0044a5' ps 2 pt 9 pi 5\n");
   fprintf (gnuplot, "set style line 4 lw 4 lc rgb '#888888' ps 2 pt 7 pi 4\n");
   fprintf (gnuplot, "plot [1:20] "
                     "'samples/%s/atomic.txt' using 2:7 with linespoints title 'Atomic (Correct)', "
                     "'samples/%s/tsx.txt' using 2:7 with linespoints title 'Intel TSX (Correct)', "
                     "'samples/%s/spinlock.txt' using 2:7 with linespoints title 'Spinlock (Correct)', "
                     "'samples/%s/addq.txt' using 2:7 with linespoints title 'Addq (Incorrect)'\n",
                     sample, sample, sample, sample);

   fclose (gnuplot);

   return EXIT_SUCCESS;
}
