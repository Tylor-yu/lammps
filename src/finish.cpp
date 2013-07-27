/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "mpi.h"
#include "math.h"
#include "string.h"
#include "stdio.h"
#include "finish.h"
#include "timer.h"
#include "universe.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "kspace.h"
#include "update.h"
#include "min.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "output.h"
#include "memory.h"
#include "accelerator_omp.h"

#ifdef LMP_USER_OMP
#include "modify.h"
#include "fix_omp.h"
#include "thr_data.h"
#endif

using namespace LAMMPS_NS;

static void print_timings(const char *label, Timer *t, enum Timer::ttype tt,
                          MPI_Comm world, const int nprocs, const int nthreads,
                          const int me, double time_loop, FILE *scr, FILE *log)
{
  double tmp, time_max, time_min;
  double time = t->get_wall(tt);
  
  double time_cpu = t->get_cpu(tt);
  if (time/time_loop < 0.001)  // insufficient timer resolution!
    time_cpu = 1.0;
  else
    time_cpu = time_cpu / time;
  if (time_cpu > nthreads) time_cpu = nthreads;

  MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
  MPI_Allreduce(&time,&time_min,1,MPI_DOUBLE,MPI_MIN,world);
  MPI_Allreduce(&time,&time_max,1,MPI_DOUBLE,MPI_MAX,world);
  time = tmp/nprocs;

  MPI_Allreduce(&time_cpu,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
  time_cpu = tmp/nprocs*100.0;

  if (me == 0) {
    time_loop = 100.0/time_loop;
    const char fmt[] = "%-8s|%- 12.5g|%- 12.5g|%- 12.5g|%6.1f | %6.2f\n";
    if (scr)
      fprintf(scr,fmt,label,time_min,time,time_max,time_cpu,time*time_loop);
    if (log)
      fprintf(log,fmt,label,time_min,time,time_max,time_cpu,time*time_loop);
  }
}

/* ---------------------------------------------------------------------- */

#ifdef LMP_USER_OMP
static void omp_times(FixOMP *fix, const char *label, const int ttype,
                      const int nthreads, FILE *scr, FILE *log)
{
  const char fmt[] = "%-8s|%- 12.5g|%- 12.5g|%- 12.5g|%6.1f |%6.2f%%\n";
  double time_min, time_max, time_total, time_avg, time_std;

  time_min=1.0e100;
  time_max=-1.0e100;
  time_total=time_avg=time_std=0.0;

  for (int i=0; i < nthreads; ++i) {
    ThrData *thr = fix->get_thr(i);
    double tmp=thr->get_time(ttype);
    time_min = MIN(time_min,tmp);
    time_max = MAX(time_max,tmp);
    time_avg += tmp;
    time_std += tmp*tmp;
    time_total += thr->get_time(ThrData::TIME_TOTAL);
  }

  time_avg /= nthreads;
  time_std /= nthreads;
  time_total = 100.0*nthreads/time_total;

  if (time_avg > 1.0e-10)
    time_std = sqrt(time_std/time_avg - time_avg)*100.0;
  else
    time_std = 0.0;

  if (scr) fprintf(scr,fmt,label,time_min,time_avg,time_max,time_std,
                   time_avg*time_total);
  if (log) fprintf(log,fmt,label,time_min,time_avg,time_max,time_std,
                   time_avg*time_total);
}
#endif


/* ---------------------------------------------------------------------- */

Finish::Finish(LAMMPS *lmp) : Pointers(lmp) {}

/* ---------------------------------------------------------------------- */

void Finish::end(int flag)
{
  int i,m,nneigh,nneighfull;
  int histo[10];
  int minflag,prdflag,tadflag,timeflag,fftflag,histoflag,neighflag;
  double time,tmp,ave,max,min;
  double time_loop,time_other,cpu_loop;

  int me,nprocs;
  MPI_Comm_rank(world,&me);
  MPI_Comm_size(world,&nprocs);

  // recompute natoms in case atoms have been lost

  bigint nblocal = atom->nlocal;
  MPI_Allreduce(&nblocal,&atom->natoms,1,MPI_LMP_BIGINT,MPI_SUM,world);

  // choose flavors of statistical output
  // flag determines caller
  // flag = 0 = just loop summary
  // flag = 1 = dynamics or minimization
  // flag = 2 = PRD
  // flag = 3 = TAD
  // turn off neighflag for Kspace partition of verlet/split integrator

  minflag = prdflag = tadflag = timeflag = fftflag = histoflag = neighflag = 0;
  time_loop = cpu_loop = time_other = 0.0;

  if (flag == 1) {
    if (update->whichflag == 2) minflag = 1;
    timeflag = histoflag = 1;
    neighflag = 1;
    if (update->whichflag == 1 &&
        strcmp(update->integrate_style,"verlet/split") == 0 &&
        universe->iworld == 1) neighflag = 0;
    if (force->kspace && force->kspace_match("pppm",0)
        && force->kspace->fftbench) fftflag = 1;
  }
  if (flag == 2) prdflag = histoflag = neighflag = 1;
  if (flag == 3) tadflag = histoflag = neighflag = 1;

  // loop stats

  if (timer->has_loop()) {
    
    // overall loop time

    time_loop = timer->get_wall(Timer::TOTAL);
    cpu_loop = timer->get_cpu(Timer::TOTAL);
    MPI_Allreduce(&time_loop,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time_loop = tmp/nprocs;
    MPI_Allreduce(&cpu_loop,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    cpu_loop = tmp/nprocs;
    if (time_loop > 0.0) cpu_loop = cpu_loop/time_loop*100.0;
 
    if (me == 0) {
      int ntasks = nprocs * comm->nthreads;

#ifdef LMP_USER_OMP
      const char fmt[] = "Loop time of %g on %d procs "
        "for %d steps with " BIGINT_FORMAT " atoms\n"
        "%6.1f%% CPU use with %d MPI tasks x %d OpenMP threads\n";
      if (screen) fprintf(screen,fmt,time_loop,ntasks,update->nsteps,
                          atom->natoms,cpu_loop,nprocs,comm->nthreads);
      if (logfile) fprintf(logfile,fmt,time_loop,ntasks,update->nsteps,
                           atom->natoms,cpu_loop,nprocs,comm->nthreads);
#else
      const char fmt[] = "Loop time of %g on %d procs "
        "for %d steps with " BIGINT_FORMAT " atoms\n"
        "%6.1f%% CPU use with %d MPI tasks\n";
      if (screen) fprintf(screen,fmt,time_loop,ntasks,update->nsteps,
                          atom->natoms,cpu_loop,nprocs);
      if (logfile) fprintf(logfile,fmt,time_loop,ntasks,update->nsteps,
                           atom->natoms,cpu_loop,nprocs);
#endif

      // Gromacs/NAMD-like performance metric for MD with suitable units

      if ( timeflag && !minflag && !prdflag && !tadflag &&
           (update->nsteps > 0) &&
           ((strcmp(update->unit_style,"metal") == 0) ||
            (strcmp(update->unit_style,"real") == 0)) ) {
        float t_step, ns_day, hrs_ns, tps, one_fs = 1.0;

        // conversion factor to femtoseconds for suitable units

        if (strcmp(update->unit_style,"metal") == 0) one_fs = 0.001;
        t_step = (float)time_loop / ((float) update->nsteps);
        tps = 1.0/t_step;
        hrs_ns = t_step / update->dt * 1000000.0 * one_fs / 60.0 / 60.0;
        ns_day = 24.0 * 60.0 * 60.0 / t_step * update->dt / one_fs / 1000000.0;

        const char perf[] = 
          "Performance: %.3f ns/day  %.3f hours/ns  %.3f timesteps/s\n";
        if (screen) fprintf(screen,perf,ns_day, hrs_ns, tps);
        if (logfile) fprintf(logfile, perf, ns_day, hrs_ns, tps);
      }
    }
  }

  // avoid division by zero for very short runs

  if (time_loop == 0.0) time_loop = 1.0;
  if (cpu_loop == 0.0) cpu_loop = 100.0;

  // get "Other" wall time for later use

  if (timer->has_normal()) {
    time_other = timer->get_wall(Timer::TOTAL) -
      (timer->get_wall(Timer::PAIR) + timer->get_wall(Timer::BOND) + 
       timer->get_wall(Timer::KSPACE) + timer->get_wall(Timer::NEIGH) +
       timer->get_wall(Timer::COMM) + timer->get_wall(Timer::OUTPUT) +
       timer->get_wall(Timer::MODIFY) + timer->get_wall(Timer::SYNC));
  }
   
  // minimization stats

  if (minflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    if (me == 0) {
      if (screen) {
        fprintf(screen,"Minimization stats:\n");
        fprintf(screen,"  Stopping criterion = %s\n",
                update->minimize->stopstr);
        fprintf(screen,"  Energy initial, next-to-last, final = \n"
                "    %18.12g %18.12g %18.12g\n",
                update->minimize->einitial,update->minimize->eprevious,
                update->minimize->efinal);
        fprintf(screen,"  Force two-norm initial, final = %g %g\n",
                update->minimize->fnorm2_init,update->minimize->fnorm2_final);
        fprintf(screen,"  Force max component initial, final = %g %g\n",
                update->minimize->fnorminf_init,
                update->minimize->fnorminf_final);
        fprintf(screen,"  Final line search alpha, max atom move = %g %g\n",
                update->minimize->alpha_final,
                update->minimize->alpha_final*
                update->minimize->fnorminf_final);
        fprintf(screen,"  Iterations, force evaluations = %d %d\n",
                update->minimize->niter,update->minimize->neval);
      }
      if (logfile) {
        fprintf(logfile,"Minimization stats:\n");
        fprintf(logfile,"  Stopping criterion = %s\n",
                update->minimize->stopstr);
        fprintf(logfile,"  Energy initial, next-to-last, final = \n"
                "    %18.12g %18.12g %18.12g\n",
                update->minimize->einitial,update->minimize->eprevious,
                update->minimize->efinal);
        fprintf(logfile,"  Force two-norm initial, final = %g %g\n",
                update->minimize->fnorm2_init,update->minimize->fnorm2_final);
        fprintf(logfile,"  Force max component initial, final = %g %g\n",
                update->minimize->fnorminf_init,
                update->minimize->fnorminf_final);
        fprintf(logfile,"  Final line search alpha, max atom move = %g %g\n",
                update->minimize->alpha_final,
                update->minimize->alpha_final*
                update->minimize->fnorminf_final);
        fprintf(logfile,"  Iterations, force evaluations = %d %d\n",
                update->minimize->niter,update->minimize->neval);
      }
    }
  }

  // PRD stats using PAIR,BOND,KSPACE for dephase,dynamics,quench

  if (prdflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    if (screen) fprintf(screen,"PRD stats:\n");
    if (logfile) fprintf(logfile,"PRD stats:\n");

    time = timer->get_wall(Timer::PAIR);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Dephase  time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Dephase  time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = timer->get_wall(Timer::BOND);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Dynamics time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Dynamics time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = timer->get_wall(Timer::KSPACE);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Quench   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Quench   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = time_other;
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Other    time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Other    time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }
  }

  // TAD stats using PAIR,BOND,KSPACE for neb,dynamics,quench

  if (tadflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    if (screen) fprintf(screen,"TAD stats:\n");
    if (logfile) fprintf(logfile,"TAD stats:\n");

    time = timer->get_wall(Timer::PAIR);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  NEB      time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  NEB      time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = timer->get_wall(Timer::BOND);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Dynamics time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Dynamics time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = timer->get_wall(Timer::KSPACE);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Quench   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Quench   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }


    time = timer->get_wall(Timer::COMM);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Comm     time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Comm     time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }


    time = timer->get_wall(Timer::OUTPUT);
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Output   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Output   time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }

    time = time_other;
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"  Other    time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"  Other    time (%%) = %g (%g)\n",
                time,time/time_loop*100.0);
    }
  }

  // timing breakdowns

  if (timeflag && timer->has_normal()) {
    const int nthreads = comm->nthreads;
    if (me == 0) {
      const char hdr[] = "\nMPI task wallclock breakdown\n"
        "Section |  min time  |  avg time  |  max time  |  %CPU |  total\n"
        "---------------------------------------------------------------\n";
      if (screen)  fputs(hdr,screen);
      if (logfile) fputs(hdr,logfile);
    }

    print_timings("Pair",timer,Timer::PAIR, world,nprocs,
                  nthreads,me,time_loop,screen,logfile);

    if (atom->molecular)
      print_timings("Bond",timer,Timer::BOND,world,nprocs,
                    nthreads,me,time_loop,screen,logfile);
    
    if (force->kspace)
      print_timings("Kspace",timer,Timer::KSPACE,world,nprocs,
                    nthreads,me,time_loop,screen,logfile);

    print_timings("Neigh",timer,Timer::NEIGH,world,nprocs,
                  nthreads,me,time_loop,screen,logfile);
    print_timings("Comm",timer,Timer::COMM,world,nprocs,
                  nthreads,me,time_loop,screen,logfile);
    print_timings("Output",timer,Timer::OUTPUT,world,nprocs,
                  nthreads,me,time_loop,screen,logfile);
    print_timings("Modify",timer,Timer::MODIFY,world,nprocs,
                  nthreads,me,time_loop,screen,logfile);
    if (timer->has_sync())
      print_timings("Sync",timer,Timer::SYNC,world,nprocs,
                    nthreads,me,time_loop,screen,logfile);

    time = time_other;
    MPI_Allreduce(&time,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time = tmp/nprocs;
    if (me == 0) {
      if (screen)
        fprintf(screen,"Other   |            |%- 12.4g|            |"
                "%6.2f |\n",time,time/time_loop*100.0);
      if (logfile)
        fprintf(logfile,"Other   |            |%- 12.4g|            |"
                "%6.2f |\n",time,time/time_loop*100.0);
    }
  }

#ifdef LMP_USER_OMP
  const char thr_hdr_fmt[] = 
    "\nThread activity (MPI rank %d only): Total time%- 11.4g/%5.1f%%\n";
  const char thr_header[] =
    "Section |  min time  |  avg time  |  max time  |var/avg|  total\n"
    "---------------------------------------------------------------\n";

  int ifix = modify->find_fix("package_omp");
  const int nthreads = comm->nthreads;

  if ((ifix >= 0) && (nthreads > 1) && me == 0) {
    FixOMP *fixomp = static_cast<FixOMP *>(lmp->modify->fix[ifix]);
    ThrData *td = fixomp->get_thr(0);
    double thr_total = td->get_time(ThrData::TIME_TOTAL);
    if (thr_total > 0.0) {
      if (screen) {
        fprintf(screen,thr_hdr_fmt,me,thr_total,thr_total/time_loop*100.0);
        fputs(thr_header,screen);
      }
      if (logfile) {
        fprintf(logfile,thr_hdr_fmt,me,thr_total,thr_total/time_loop*100.0);
        fputs(thr_header,logfile);
      }

      omp_times(fixomp,"Pair",ThrData::TIME_PAIR,nthreads,screen,logfile);
      if (atom->molecular) omp_times(fixomp,"Bond",ThrData::TIME_BOND,
                                     nthreads,screen,logfile);
      if (force->kspace) omp_times(fixomp,"Kspace",ThrData::TIME_KSPACE,
                                   nthreads,screen,logfile);
      omp_times(fixomp,"Neigh",ThrData::TIME_NEIGH,nthreads,screen,logfile);
      omp_times(fixomp,"Reduce",ThrData::TIME_REDUCE,nthreads,screen,logfile);
      omp_times(fixomp,"Modify",ThrData::TIME_MODIFY,nthreads,screen,logfile);
    }
  }
#endif

  // FFT timing statistics
  // time3d,time1d = total time during run for 3d and 1d FFTs
  // loop on timing() until nsample FFTs require at least 1.0 CPU sec
  // time_kspace may be 0.0 if another partition is doing Kspace

  if (fftflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    int nsteps = update->nsteps;

    double time3d;
    int nsample = 1;
    int nfft = force->kspace->timing_3d(nsample,time3d);
    while (time3d < 1.0) {
      nsample *= 2;
      nfft = force->kspace->timing_3d(nsample,time3d);
    }

    time3d = nsteps * time3d / nsample;
    MPI_Allreduce(&time3d,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time3d = tmp/nprocs;

    double time1d;
    nsample = 1;
    nfft = force->kspace->timing_1d(nsample,time1d);
    while (time1d < 1.0) {
      nsample *= 2;
      nfft = force->kspace->timing_1d(nsample,time1d);
    }
    
    time1d = nsteps * time1d / nsample;
    MPI_Allreduce(&time1d,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time1d = tmp/nprocs;

    double time_kspace = timer->get_wall(Timer::KSPACE);
    MPI_Allreduce(&time_kspace,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
    time_kspace = tmp/nprocs;

    double ntotal = 1.0 * force->kspace->nx_pppm *
      force->kspace->ny_pppm * force->kspace->nz_pppm;
    double nflops = 5.0 * ntotal * log(ntotal);

    double fraction,flop3,flop1;
    if (nsteps) {
      if (time_kspace) fraction = time3d/time_kspace*100.0;
      else fraction = 0.0;
      flop3 = nfft*nflops/1.0e9/(time3d/nsteps);
      flop1 = nfft*nflops/1.0e9/(time1d/nsteps);
    } else fraction = flop3 = flop1 = 0.0;

    if (me == 0) {
      if (screen) {
        fprintf(screen,"FFT time (%% of Kspce) = %g (%g)\n",time3d,fraction);
        fprintf(screen,"FFT Gflps 3d (1d only) = %g %g\n",flop3,flop1);
      }
      if (logfile) {
        fprintf(logfile,"FFT time (%% of Kspce) = %g (%g)\n",time3d,fraction);
        fprintf(logfile,"FFT Gflps 3d (1d only) = %g %g\n",flop3,flop1);
      }
    }
  }

  if (histoflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    tmp = atom->nlocal;
    stats(1,&tmp,&ave,&max,&min,10,histo);
    if (me == 0) {
      if (screen) {
        fprintf(screen,"Nlocal:    %g ave %g max %g min\n",ave,max,min);
        fprintf(screen,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(screen," %d",histo[i]);
        fprintf(screen,"\n");
      }
      if (logfile) {
        fprintf(logfile,"Nlocal:    %g ave %g max %g min\n",ave,max,min);
        fprintf(logfile,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(logfile," %d",histo[i]);
        fprintf(logfile,"\n");
      }
    }

    tmp = atom->nghost;
    stats(1,&tmp,&ave,&max,&min,10,histo);
    if (me == 0) {
      if (screen) {
        fprintf(screen,"Nghost:    %g ave %g max %g min\n",ave,max,min);
        fprintf(screen,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(screen," %d",histo[i]);
        fprintf(screen,"\n");
      }
      if (logfile) {
        fprintf(logfile,"Nghost:    %g ave %g max %g min\n",ave,max,min);
        fprintf(logfile,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(logfile," %d",histo[i]);
        fprintf(logfile,"\n");
      }
    }

    // find a non-skip neighbor list containing half the pairwise interactions
    // count neighbors in that list for stats purposes

    for (m = 0; m < neighbor->old_nrequest; m++)
      if ((neighbor->old_requests[m]->half ||
           neighbor->old_requests[m]->gran ||
           neighbor->old_requests[m]->respaouter ||
           neighbor->old_requests[m]->half_from_full) &&
          neighbor->old_requests[m]->skip == 0 &&
          neighbor->lists[m]->numneigh) break;

    nneigh = 0;
    if (m < neighbor->old_nrequest) {
      int inum = neighbor->lists[m]->inum;
      int *ilist = neighbor->lists[m]->ilist;
      int *numneigh = neighbor->lists[m]->numneigh;
      for (i = 0; i < inum; i++)
        nneigh += numneigh[ilist[i]];
    }

    tmp = nneigh;
    stats(1,&tmp,&ave,&max,&min,10,histo);
    if (me == 0) {
      if (screen) {
        fprintf(screen,"Neighs:    %g ave %g max %g min\n",ave,max,min);
        fprintf(screen,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(screen," %d",histo[i]);
        fprintf(screen,"\n");
      }
      if (logfile) {
        fprintf(logfile,"Neighs:    %g ave %g max %g min\n",ave,max,min);
        fprintf(logfile,"Histogram:");
        for (i = 0; i < 10; i++) fprintf(logfile," %d",histo[i]);
        fprintf(logfile,"\n");
      }
    }

    // find a non-skip neighbor list containing full pairwise interactions
    // count neighbors in that list for stats purposes

    for (m = 0; m < neighbor->old_nrequest; m++)
      if (neighbor->old_requests[m]->full &&
          neighbor->old_requests[m]->skip == 0) break;

    nneighfull = 0;
    if (m < neighbor->old_nrequest) {
      if ((neighbor->lists[m]->numneigh) > 0) {
        int inum = neighbor->lists[m]->inum;
        int *ilist = neighbor->lists[m]->ilist;
        int *numneigh = neighbor->lists[m]->numneigh;
        for (i = 0; i < inum; i++)
          nneighfull += numneigh[ilist[i]];
      }

      tmp = nneighfull;
      stats(1,&tmp,&ave,&max,&min,10,histo);
      if (me == 0) {
        if (screen) {
          fprintf(screen,"FullNghs:  %g ave %g max %g min\n",ave,max,min);
          fprintf(screen,"Histogram:");
          for (i = 0; i < 10; i++) fprintf(screen," %d",histo[i]);
          fprintf(screen,"\n");
        }
        if (logfile) {
          fprintf(logfile,"FullNghs:  %g ave %g max %g min\n",ave,max,min);
          fprintf(logfile,"Histogram:");
          for (i = 0; i < 10; i++) fprintf(logfile," %d",histo[i]);
          fprintf(logfile,"\n");
        }
      }
    }
  }

  if (neighflag) {
    if (me == 0) {
      if (screen) fprintf(screen,"\n");
      if (logfile) fprintf(logfile,"\n");
    }

    tmp = MAX(nneigh,nneighfull);
    double nall;
    MPI_Allreduce(&tmp,&nall,1,MPI_DOUBLE,MPI_SUM,world);

    int nspec;
    double nspec_all;
    if (atom->molecular) {
      nspec = 0;
      int nlocal = atom->nlocal;
      for (i = 0; i < nlocal; i++) nspec += atom->nspecial[i][2];
      tmp = nspec;
      MPI_Allreduce(&tmp,&nspec_all,1,MPI_DOUBLE,MPI_SUM,world);
    }

    if (me == 0) {
      if (screen) {
        if (nall < 2.0e9)
          fprintf(screen,
                  "Total # of neighbors = %d\n",static_cast<int> (nall));
        else fprintf(screen,"Total # of neighbors = %g\n",nall);
        if (atom->natoms > 0)
          fprintf(screen,"Ave neighs/atom = %g\n",nall/atom->natoms);
        if (atom->molecular && atom->natoms > 0)
          fprintf(screen,"Ave special neighs/atom = %g\n",
                  nspec_all/atom->natoms);
        fprintf(screen,"Neighbor list builds = " BIGINT_FORMAT "\n",
                neighbor->ncalls);
        fprintf(screen,"Dangerous builds = " BIGINT_FORMAT "\n",
                neighbor->ndanger);
      }
      if (logfile) {
        if (nall < 2.0e9)
          fprintf(logfile,
                  "Total # of neighbors = %d\n",static_cast<int> (nall));
        else fprintf(logfile,"Total # of neighbors = %g\n",nall);
        if (atom->natoms > 0)
          fprintf(logfile,"Ave neighs/atom = %g\n",nall/atom->natoms);
        if (atom->molecular && atom->natoms > 0)
          fprintf(logfile,"Ave special neighs/atom = %g\n",
                  nspec_all/atom->natoms);
        fprintf(logfile,"Neighbor list builds = " BIGINT_FORMAT "\n",
                neighbor->ncalls);
        fprintf(logfile,"Dangerous builds = " BIGINT_FORMAT "\n",
                neighbor->ndanger);
      }
    }
  }

  if (logfile) fflush(logfile);
}

/* ---------------------------------------------------------------------- */

void Finish::stats(int n, double *data,
                   double *pave, double *pmax, double *pmin,
                   int nhisto, int *histo)
{
  int i,m;
  int *histotmp;

  double min = 1.0e20;
  double max = -1.0e20;
  double ave = 0.0;
  for (i = 0; i < n; i++) {
    ave += data[i];
    if (data[i] < min) min = data[i];
    if (data[i] > max) max = data[i];
  }

  int ntotal;
  MPI_Allreduce(&n,&ntotal,1,MPI_INT,MPI_SUM,world);
  double tmp;
  MPI_Allreduce(&ave,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
  ave = tmp/ntotal;
  MPI_Allreduce(&min,&tmp,1,MPI_DOUBLE,MPI_MIN,world);
  min = tmp;
  MPI_Allreduce(&max,&tmp,1,MPI_DOUBLE,MPI_MAX,world);
  max = tmp;

  for (i = 0; i < nhisto; i++) histo[i] = 0;

  double del = max - min;
  for (i = 0; i < n; i++) {
    if (del == 0.0) m = 0;
    else m = static_cast<int> ((data[i]-min)/del * nhisto);
    if (m > nhisto-1) m = nhisto-1;
    histo[m]++;
  }

  memory->create(histotmp,nhisto,"finish:histotmp");
  MPI_Allreduce(histo,histotmp,nhisto,MPI_INT,MPI_SUM,world);
  for (i = 0; i < nhisto; i++) histo[i] = histotmp[i];
  memory->destroy(histotmp);

  *pave = ave;
  *pmax = max;
  *pmin = min;
}
