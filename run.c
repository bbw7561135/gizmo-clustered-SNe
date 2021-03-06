#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>

#include "allvars.h"
#include "proto.h"


/*! \file run.c
 *  \brief  iterates over timesteps, main loop
 */
/*
 * This file was originally part of the GADGET3 code developed by
 * Volker Springel (volker.springel@h-its.org). The code has been modified
 * in part (adding/removing calls, re-ordering some routines, and 
 * adding hooks to new elements such as particle splitting, as necessary)
 * by Phil Hopkins (phopkins@caltech.edu) for GIZMO.
 */


/*! This routine contains the main simulation loop that iterates over
 * single timesteps. The loop terminates when the cpu-time limit is
 * reached, when a `stop' file is found in the output directory, or
 * when the simulation ends because we arrived at TimeMax.
 */
void run(void)
{
    CPU_Step[CPU_MISC] += measure_time();
    
    if(RestartFlag != 1)		/* need to compute forces at initial synchronization time, unless we restarted from restart files */
    {
        output_log_messages();
        
        domain_Decomposition(0, 0, 0);
        
        set_non_standard_physics_for_current_time();
        
        compute_grav_accelerations();	/* compute gravitational accelerations for synchronous particles */
        
        compute_hydro_densities_and_forces();	/* densities, gradients, & hydro-accels for synchronous particles */
        
        calculate_non_standard_physics();	/* source terms are here treated in a strang-split fashion */
    }
    
    while(1)			/* main timestep iteration loop */
    {
        compute_statistics();	/* regular statistics outputs (like total energy) */
        
        write_cpu_log();		/* output some CPU usage log-info (accounts for everything needed up to the current sync-point) */
        
        if(All.Ti_Current >= TIMEBASE)	/* check whether we reached the final time */
        {
            if(ThisTask == 0)
                printf("\nFinal time=%g reached. Simulation ends.\n", All.TimeMax);
            
            restart(0);		/* write a restart file to allow continuation of the run for a larger value of TimeMax */
            
            if(All.Ti_lastoutput != All.Ti_Current)	/* make a snapshot at the final time in case none has produced at this time */
                savepositions(All.SnapshotFileCount++);	/* this will be overwritten if All.TimeMax is increased and the run is continued */
            
            break;
        }
        
        find_timesteps();		/* find-timesteps */
        
        do_first_halfstep_kick();	/* half-step kick at beginning of timestep for synchronous particles */
        
        find_next_sync_point_and_drift();	/* find next synchronization point and drift particles to this time.
                                             * If needed, this function will also write an output file
                                             * at the desired time.
                                             */
        
        output_log_messages();	/* write some info to log-files */
        
        set_non_standard_physics_for_current_time();	/* update auxiliary physics for current time */
        
        
        if(GlobNumForceUpdate > All.TreeDomainUpdateFrequency * All.TotNumPart)	/* check whether we have a big step */
        {
            domain_Decomposition(0, 0, 1);	/* do domain decomposition if step is big enough, and set new list of active particles  */
        }
        else
        {
            force_update_tree();	/* update tree dynamically with kicks of last step so that it can be reused */
            
            make_list_of_active_particles();	/* now we can set the new chain list of active particles */
        }
        
        compute_grav_accelerations();	/* compute gravitational accelerations for synchronous particles */


#if (defined(FLAG_NOT_IN_PUBLIC_CODE) || defined(FLAG_NOT_IN_PUBLIC_CODE) || defined(FLAG_NOT_IN_PUBLIC_CODE) || defined(GALSF_FB_LUPI))
        /* flag particles which will be feedback centers, so kernel lengths can be computed for them */
        determine_where_SNe_occur();
#endif
        
        compute_hydro_densities_and_forces();	/* densities, gradients, & hydro-accels for synchronous particles */
        
        do_second_halfstep_kick();	/* this does the half-step kick at the end of the timestep */
        
        if(ThisTask == 0)
        {
            printf("about to enter `calculate_non_standard_physics` after 2nd halfkick\n");
        }
        calculate_non_standard_physics();	/* source terms are here treated in a strang-split fashion */
        if(ThisTask == 0)
        {
            printf("just exited `calculate_non_standard_physics`\n");
        }        

        if(ThisTask == 0)
        {
            printf("about to enter stopflag check\n");
        }

        /* Check whether we need to interrupt the run */
        int stopflag = 0;
        if(ThisTask == 0)
        {
            FILE *fd;
            char stopfname[1000];
            sprintf(stopfname, "%sstop", All.OutputDir);
            if((fd = fopen(stopfname, "r")))	/* Is the stop-file present? If yes, interrupt the run. */
            {
                fclose(fd);
                stopflag = 1;
                unlink(stopfname);
            }
            
            if(CPUThisRun > 0.85 * All.TimeLimitCPU)	/* are we running out of CPU-time ? If yes, interrupt run. */
            {
                printf("reaching time-limit. stopping.\n");
                stopflag = 2;
            }
        }
        
        MPI_Bcast(&stopflag, 1, MPI_INT, 0, MPI_COMM_WORLD);
        
        if(stopflag)
        {
            restart(0);		/* write restart file */
            MPI_Barrier(MPI_COMM_WORLD);
            
            if(stopflag == 2 && ThisTask == 0)
            {
                FILE *fd;
                char contfname[1000];
                sprintf(contfname, "%scont", All.OutputDir);
                if((fd = fopen(contfname, "w")))
                    fclose(fd);
                
                if(All.ResubmitOn)
                    execute_resubmit_command();
            }
            return;
        }
        
        if(ThisTask == 0)
        {
            /* is it time to write one of the regularly space restart-files? */
            if((CPUThisRun - All.TimeLastRestartFile) >= All.CpuTimeBetRestartFile)
            {
                All.TimeLastRestartFile = CPUThisRun;
                stopflag = 3;
            }
            else
                stopflag = 0;
        }
        
        MPI_Bcast(&stopflag, 1, MPI_INT, 0, MPI_COMM_WORLD);
        
        if(stopflag == 3)
        {
            restart(0);		/* write an occasional restart file */
            stopflag = 0;
            All.TimeLastRestartFile += report_time();
        }

        if(ThisTask == 0)
        {
            printf("about to end stopflag check\n");
        }
       
        if(ThisTask == 0)
        {
            printf("about to enter `set_random_numbers`\n");
        } 
        set_random_numbers();	/* draw a new list of random numbers */
        if(ThisTask == 0)
        {
            printf("just exited `set_random_numbers`\n");
        }

        if(ThisTask == 0)
        {
            printf("about to enter `report_memory_usage`\n");
        }
        report_memory_usage(&HighMark_run, "RUN");
        if(ThisTask == 0)
        {
            printf("just exited `report_memory_usage`\n");
        }       

        if(ThisTask == 0)
        {
            printf("finished main timestep iteration loop\n");
        }
    }
    
}




void set_non_standard_physics_for_current_time(void)
{
#if defined(COOLING)
    /* set UV background for the current time */
    IonizeParams();
#endif
    


    
}



void calculate_non_standard_physics(void)
{
#ifdef PARTICLE_EXCISION
    apply_excision();
#endif
    
#ifdef GALSF
    /* PFH set of feedback routines */
    compute_stellar_feedback();
#endif


#ifdef BH_LUPI
    CPU_Step[CPU_MISC] += measure_time();
    blackhole_accretion_calc();
    blackhole_feedback_calc();
    CPU_Step[CPU_BLACKHOLES] += measure_time();
#endif
    
#if defined(FLAG_NOT_IN_PUBLIC_CODE) || defined(FLAG_NOT_IN_PUBLIC_CODE)
#endif // ifdef FLAG_NOT_IN_PUBLIC_CODE or FLAG_NOT_IN_PUBLIC_CODE
    
    if(ThisTask == 0)
    {
        printf("about to enter cooling\n");
    }    
#ifdef COOLING	/**** radiative cooling and star formation *****/
#ifdef GALSF
    if(ThisTask == 0)
    {
        printf("specifically entering `cooling_and_starformation`\n");
    }
    cooling_and_starformation(); // standard cooling+star formation routine //
#else // ifdef GALSF else
    if(ThisTask == 0)
    { 
        printf("specifically entering `cooling_only`\n");
    }
    cooling_only();
#endif // closes if GALSF
    CPU_Step[CPU_COOLINGSFR] += measure_time(); // finish time calc for SFR+cooling
#endif /*ends COOLING */
    
    if(ThisTask == 0)
    {
       printf("just exited cooling\n");
    }
    
    
    
}



void compute_statistics(void)
{
    if(ThisTask == 0)
    {
        printf("starting compute_statistics\n");
    }

    if((All.Time - All.TimeLastStatistics) >= All.TimeBetStatistics)
    {
#if !defined(EVALPOTENTIAL)          // DAA: compute_potential is not defined if EVALPOTENTIAL is on... check!
#ifdef COMPUTE_POTENTIAL_ENERGY
        compute_potential();
#endif
#endif
        energy_statistics();	/* compute and output energy statistics */
        
        
        All.TimeLastStatistics += All.TimeBetStatistics;
    }

    if(ThisTask == 0)
    {
        printf("done compute_statistics\n");
    }
}



void execute_resubmit_command(void)
{
    char buf[1000];
    sprintf(buf, "%s", All.ResubmitCommand);
#ifndef NOCALLSOFSYSTEM
    system(buf);
#endif
}



/*! This function finds the next synchronization point of the system
 * (i.e. the earliest point of time any of the particles needs a force
 * computation), and drifts the system to this point of time.  If the
 * system drifts over the desired time of a snapshot file, the
 * function will drift to this moment, generate an output, and then
 * resume the drift.
 */
void find_next_sync_point_and_drift(void)
{
  int n, i, prev;
  integertime dt_bin, ti_next_for_bin, ti_next_kick, ti_next_kick_global;
  int highest_active_bin, highest_occupied_bin;
  double timeold;

  timeold = All.Time;

  All.NumCurrentTiStep++;	/* we are now moving to the next sync point */

  /* find the next kick time */
  for(n = 0, ti_next_kick = TIMEBASE, highest_occupied_bin = 0; n < TIMEBINS; n++)
    {
      if(TimeBinCount[n])
	{
	  if(n > 0)
	    {
	      highest_occupied_bin = n;
	      dt_bin = (((integertime) 1) << n);
	      ti_next_for_bin = (All.Ti_Current / dt_bin) * dt_bin + dt_bin;	/* next kick time for this timebin */
	    }
	  else
	    {
	      dt_bin = 0;
	      ti_next_for_bin = All.Ti_Current;
	    }

	  if(ti_next_for_bin < ti_next_kick)
	    ti_next_kick = ti_next_for_bin;
	}
    }

  MPI_Allreduce(&ti_next_kick, &ti_next_kick_global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);

  while(ti_next_kick_global >= All.Ti_nextoutput && All.Ti_nextoutput >= 0)
    {
      All.Ti_Current = All.Ti_nextoutput;

      if(All.ComovingIntegrationOn)
          All.Time = All.TimeBegin * exp(All.Ti_Current * All.Timebase_interval);
      else
          All.Time = All.TimeBegin + All.Ti_Current * All.Timebase_interval;

      set_cosmo_factors_for_current_time();


      move_particles(All.Ti_nextoutput);

      CPU_Step[CPU_DRIFT] += measure_time();

#ifdef OUTPUTPOTENTIAL
#if !defined(EVALPOTENTIAL) || (defined(EVALPOTENTIAL) && defined(RECOMPUTE_POTENTIAL_ON_OUTPUT))
      domain_Decomposition(0, 0, 0);

      compute_potential();
#endif
#endif


      mpi_printf("\n\n\nI found the last snapshot call...\n\n\n");
      savepositions(All.SnapshotFileCount++);	/* write snapshot file */

      All.Ti_nextoutput = find_next_outputtime(All.Ti_nextoutput + 1);
    }


  All.Previous_Ti_Current = All.Ti_Current;
  All.Ti_Current = ti_next_kick_global;

  if(All.ComovingIntegrationOn)
    All.Time = All.TimeBegin * exp(All.Ti_Current * All.Timebase_interval);
  else
    All.Time = All.TimeBegin + All.Ti_Current * All.Timebase_interval;

  set_cosmo_factors_for_current_time();
#ifdef SHEARING_BOX
    calc_shearing_box_pos_offset();
#endif


  All.TimeStep = All.Time - timeold;

  /* mark the bins that will be active */
  for(n = 1, TimeBinActive[0] = 1, NumForceUpdate = TimeBinCount[0], highest_active_bin = 0; n < TIMEBINS;
      n++)
    {
      dt_bin = (((integertime) 1) << n);
      if((ti_next_kick_global % dt_bin) == 0)
	{
	  TimeBinActive[n] = 1;
	  NumForceUpdate += TimeBinCount[n];
	  if(TimeBinCount[n])
	    highest_active_bin = n;
	}
      else
	TimeBinActive[n] = 0;
    }

  sumup_large_ints(1, &NumForceUpdate, &GlobNumForceUpdate);
  MPI_Allreduce(&highest_active_bin, &All.HighestActiveTimeBin, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&highest_occupied_bin, &All.HighestOccupiedTimeBin, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  if(GlobNumForceUpdate == All.TotNumPart)
    {
      Flag_FullStep = 1;
      if(All.HighestActiveTimeBin != All.HighestOccupiedTimeBin)
	terminate("Something is wrong with the time bins.\n");
    }
  else
    Flag_FullStep = 0;




  /* move the new set of active/synchronized particles */
  /* Note: We do not yet call make_list_of_active_particles(), since we
   * may still need to old list in the dynamic tree update
   */
  for(n = 0, prev = -1; n < TIMEBINS; n++)
    {
      if(TimeBinActive[n])
	{
	  for(i = FirstInTimeBin[n]; i >= 0; i = NextInTimeBin[i])
	    {
	      drift_particle(i, All.Ti_Current);
	    }
	}
    }

 fflush(stdout);

}


void make_list_of_active_particles(void)
{
    int i, n, prev;
    /* make a link list with the particles in the active time bins */
    FirstActiveParticle = -1;
    
    for(n = 0, prev = -1; n < TIMEBINS; n++)
    {
        if(TimeBinActive[n])
        {
            for(i = FirstInTimeBin[n]; i >= 0; i = NextInTimeBin[i])
            {
                if(P[i].Mass <= 0)
                    continue;
                
                if(prev == -1)
                    FirstActiveParticle = i;
                
                if(prev >= 0)
                    NextActiveParticle[prev] = i;
                
                prev = i;
            }
        }
    }

    if(prev >= 0)
        NextActiveParticle[prev] = -1;
}





/*! this function returns the next output time that is equal or larger to
 *  ti_curr
 */
integertime find_next_outputtime(integertime ti_curr)
{
  int i, iter = 0;
  integertime ti, ti_next;
  double next, time;

  DumpFlag = 1;
  ti_next = -1;


  if(All.OutputListOn)
    {
      for(i = 0; i < All.OutputListLength; i++)
	{
	  time = All.OutputListTimes[i];

	  if(time >= All.TimeBegin && time <= All.TimeMax)
	    {
	      if(All.ComovingIntegrationOn)
		ti = (integertime) (log(time / All.TimeBegin) / All.Timebase_interval);
	      else
		ti = (integertime) ((time - All.TimeBegin) / All.Timebase_interval);

	      if(ti >= ti_curr)
		{
		  if(ti_next == -1)
		    {
		      ti_next = ti;
		      DumpFlag = All.OutputListFlag[i];
		      if(i > All.SnapshotFileCount)
			All.SnapshotFileCount = i;
		    }

		  if(ti_next > ti)
		    {
		      ti_next = ti;
		      DumpFlag = All.OutputListFlag[i];
		      if(i > All.SnapshotFileCount)
			All.SnapshotFileCount = i;
		    }
		}
	    }
	}
    }
  else
    {
      if(All.ComovingIntegrationOn)
	{
	  if(All.TimeBetSnapshot <= 1.0)
	    {
	      printf("TimeBetSnapshot > 1.0 required for your simulation.\n");
	      endrun(13123);
	    }
	}
      else
	{
	  if(All.TimeBetSnapshot <= 0.0)
	    {
	      printf("TimeBetSnapshot > 0.0 required for your simulation.\n");
	      endrun(13123);
	    }
	}
      time = All.TimeOfFirstSnapshot;

      iter = 0;

      while(time < All.TimeBegin)
	{
	  if(All.ComovingIntegrationOn)
	    time *= All.TimeBetSnapshot;
	  else
	    time += All.TimeBetSnapshot;

	  iter++;

	  if(iter > 1000000)
	    {
	      printf("Can't determine next output time.\n");
	      endrun(110);
	    }
	}
      while(time <= All.TimeMax)
	{
	  if(All.ComovingIntegrationOn)
	    ti = (integertime) (log(time / All.TimeBegin) / All.Timebase_interval);
	  else
	    ti = (integertime) ((time - All.TimeBegin) / All.Timebase_interval);

	  if(ti >= ti_curr)
	    {
	      ti_next = ti;
	      break;
	    }

	  if(All.ComovingIntegrationOn)
	    time *= All.TimeBetSnapshot;
	  else
	    time += All.TimeBetSnapshot;

	  iter++;

	  if(iter > 1000000)
	    {
	      printf("Can't determine next output time.\n");
	      endrun(111);
	    }
	}
    }


  if(ti_next == -1)
    {
      ti_next = 2 * TIMEBASE;	/* this will prevent any further output */

      if(ThisTask == 0)
	printf("\nThere is no valid time for a further snapshot file.\n");
    }
  else
    {
      if(All.ComovingIntegrationOn)
	next = All.TimeBegin * exp(ti_next * All.Timebase_interval);
      else
	next = All.TimeBegin + ti_next * All.Timebase_interval;

      if(ThisTask == 0)
	printf("\nSetting next time for snapshot file to Time_next= %g  (DumpFlag=%d)\n\n", next, DumpFlag);

    }

  return ti_next;
}




/*! This routine writes for every synchronisation point in the timeline information to two log-files:
 * In FdInfo, we just list the timesteps that have been done, while in
 * FdTimebins we inform about the distribution of particles over the timebins, and which timebins are active on this step.
 * code is stored.
 */
void output_log_messages(void)
{
  double z;
  int i, j;
  long long tot, tot_sph;
  long long tot_count[TIMEBINS];
  long long tot_count_sph[TIMEBINS];
  long long tot_cumulative[TIMEBINS];
  int weight, corr_weight;
  double sum, avg_CPU_TimeBin[TIMEBINS], frac_CPU_TimeBin[TIMEBINS];

  sumup_large_ints(TIMEBINS, TimeBinCount, tot_count);
  sumup_large_ints(TIMEBINS, TimeBinCountSph, tot_count_sph);

  if(ThisTask == 0)
    {
      if(All.ComovingIntegrationOn)
	{
	  z = 1.0 / (All.Time) - 1;
	  fprintf(FdInfo, "\nSync-Point %d, Time: %g, Redshift: %g, Nf = %d%09d, Systemstep: %g, Dloga: %g\n",
		  All.NumCurrentTiStep, All.Time, z,
		  (int) (GlobNumForceUpdate / 1000000000), (int) (GlobNumForceUpdate % 1000000000),
		  All.TimeStep, log(All.Time) - log(All.Time - All.TimeStep));
	  printf("\nSync-Point %d, Time: %g, Redshift: %g, Systemstep: %g, Dloga: %g\n", All.NumCurrentTiStep,
		 All.Time, z, All.TimeStep, log(All.Time) - log(All.Time - All.TimeStep));
	  fprintf(FdTimebin, "\nSync-Point %d, Time: %g, Redshift: %g, Systemstep: %g, Dloga: %g\n",
		  All.NumCurrentTiStep, All.Time, z, All.TimeStep,
		  log(All.Time) - log(All.Time - All.TimeStep));
	  fflush(FdInfo);
	}
      else
	{
	  fprintf(FdInfo, "\nSync-Point %d, Time: %g, Nf = %d%09d, Systemstep: %g\n", All.NumCurrentTiStep,
		  All.Time, (int) (GlobNumForceUpdate / 1000000000), (int) (GlobNumForceUpdate % 1000000000),
		  All.TimeStep);
	  printf("\nSync-Point %d, Time: %g, Systemstep: %g\n", All.NumCurrentTiStep, All.Time, All.TimeStep);
	  fprintf(FdTimebin, "\nSync-Point %d, Time: %g, Systemstep: %g\n", All.NumCurrentTiStep, All.Time,
		  All.TimeStep);
	  fflush(FdInfo);
	}

      for(i = 1, tot_cumulative[0] = tot_count[0]; i < TIMEBINS; i++)
	tot_cumulative[i] = tot_count[i] + tot_cumulative[i - 1];


      for(i = 0; i < TIMEBINS; i++)
	{
	  for(j = 0, sum = 0; j < All.CPU_TimeBinCountMeasurements[i]; j++)
	    sum += All.CPU_TimeBinMeasurements[i][j];
	  if(All.CPU_TimeBinCountMeasurements[i])
	    avg_CPU_TimeBin[i] = sum / All.CPU_TimeBinCountMeasurements[i];
	  else
	    avg_CPU_TimeBin[i] = 0;
	}

      for(i = All.HighestOccupiedTimeBin, weight = 1, sum = 0; i >= 0 && tot_count[i] > 0; i--, weight *= 2)
	{
	  if(weight > 1)
	    corr_weight = weight / 2;
	  else
	    corr_weight = weight;

	  frac_CPU_TimeBin[i] = corr_weight * avg_CPU_TimeBin[i];
	  sum += frac_CPU_TimeBin[i];
	}

      for(i = All.HighestOccupiedTimeBin; i >= 0 && tot_count[i] > 0; i--)
	{
	  if(sum)
	    frac_CPU_TimeBin[i] /= sum;
	}


      printf
	("Occupied timebins: non-cells     cells       dt                 cumulative A D    avg-time  cpu-frac\n");
      fprintf(FdTimebin,
	      "Occupied timebins: non-cells     cells       dt                 cumulative A D    avg-time  cpu-frac\n");
      for(i = TIMEBINS - 1, tot = tot_sph = 0; i >= 0; i--)
	if(tot_count_sph[i] > 0 || tot_count[i] > 0)
	  {
	    printf(" %c  bin=%2d      %10llu  %10llu   %16.12f       %10llu %c %c  %10.2f    %5.1f%%\n",
		   TimeBinActive[i] ? 'X' : ' ',
		   i, tot_count[i] - tot_count_sph[i], tot_count_sph[i],
		   i > 0 ? (((integertime) 1) << i) * All.Timebase_interval : 0.0, tot_cumulative[i],
		   (i == All.HighestActiveTimeBin) ? '<' : ' ',
		   (tot_cumulative[i] > All.TreeDomainUpdateFrequency * All.TotNumPart) ? '*' : ' ',
		   avg_CPU_TimeBin[i], 100.0 * frac_CPU_TimeBin[i]);

	    fprintf(FdTimebin,
		    " %c  bin=%2d      %10llu  %10llu   %16.12f       %10llu %c %c  %10.2f    %5.1f%%\n",
		    TimeBinActive[i] ? 'X' : ' ', i, tot_count[i] - tot_count_sph[i], tot_count_sph[i],
		    i > 0 ? (((integertime) 1) << i) * All.Timebase_interval : 0.0, tot_cumulative[i],
		    (i == All.HighestActiveTimeBin) ? '<' : ' ',
		    (tot_cumulative[i] > All.TreeDomainUpdateFrequency * All.TotNumPart) ? '*' : ' ',
		    avg_CPU_TimeBin[i], 100.0 * frac_CPU_TimeBin[i]);

	    if(TimeBinActive[i])
	      {
		tot += tot_count[i];
		tot_sph += tot_count_sph[i];
	      }
	  }
      printf("               ------------------------\n");
      fprintf(FdTimebin, "               ------------------------\n");

	{
	  printf("Total active:   %10llu  %10llu    Sum: %10llu\n", tot - tot_sph, tot_sph, tot);
	  fprintf(FdTimebin, "Total active:   %10llu  %10llu    Sum: %10llu\n", tot - tot_sph, tot_sph, tot);

	}
      fprintf(FdTimebin, "\n");
      fflush(FdTimebin);

    }

  output_extra_log_messages();
}




void write_cpu_log(void)
{
  double max_CPU_Step[CPU_PARTS], avg_CPU_Step[CPU_PARTS], t0, t1, tsum;
  int i;

  CPU_Step[CPU_MISC] += measure_time();

  for(i = 1, CPU_Step[0] = 0; i < CPU_PARTS; i++)
    CPU_Step[0] += CPU_Step[i];

  MPI_Reduce(CPU_Step, max_CPU_Step, CPU_PARTS, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
  MPI_Reduce(CPU_Step, avg_CPU_Step, CPU_PARTS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);


  if(ThisTask == 0)
    {
      for(i = 0; i < CPU_PARTS; i++)
	avg_CPU_Step[i] /= NTask;

      put_symbol(0.0, 1.0, '#');

      for(i = 1, tsum = 0.0; i < CPU_PARTS; i++)
	{
	  if(max_CPU_Step[i] > 0)
	    {
	      t0 = tsum;
	      t1 = tsum + avg_CPU_Step[i] * (avg_CPU_Step[i] / max_CPU_Step[i]);
	      put_symbol(t0 / avg_CPU_Step[0], t1 / avg_CPU_Step[0], CPU_Symbol[i]);
	      tsum += t1 - t0;

	      t0 = tsum;
	      t1 = tsum + avg_CPU_Step[i] * ((max_CPU_Step[i] - avg_CPU_Step[i]) / max_CPU_Step[i]);
	      put_symbol(t0 / avg_CPU_Step[0], t1 / avg_CPU_Step[0], CPU_SymbolImbalance[i]);
	      tsum += t1 - t0;
	    }
	}

      put_symbol(tsum / max_CPU_Step[0], 1.0, '-');

      fprintf(FdBalance, "Step=%7d  sec=%10.3f  Nf=%2d%09d  %s\n", All.NumCurrentTiStep, max_CPU_Step[0],
	      (int) (GlobNumForceUpdate / 1000000000), (int) (GlobNumForceUpdate % 1000000000), CPU_String);
      fflush(FdBalance);

      if(All.CPU_TimeBinCountMeasurements[All.HighestActiveTimeBin] == NUMBER_OF_MEASUREMENTS_TO_RECORD)
	{
	  All.CPU_TimeBinCountMeasurements[All.HighestActiveTimeBin]--;
	  memmove(&All.CPU_TimeBinMeasurements[All.HighestActiveTimeBin][0],
		  &All.CPU_TimeBinMeasurements[All.HighestActiveTimeBin][1],
		  (NUMBER_OF_MEASUREMENTS_TO_RECORD - 1) * sizeof(double));
	}

      All.CPU_TimeBinMeasurements[All.HighestActiveTimeBin][All.CPU_TimeBinCountMeasurements
							    [All.HighestActiveTimeBin]++] = max_CPU_Step[0];
    }

  CPUThisRun += CPU_Step[0];

  for(i = 0; i < CPU_PARTS; i++)
    CPU_Step[i] = 0;

  if(ThisTask == 0)
    {
      for(i = 0; i < CPU_PARTS; i++)
	All.CPU_Sum[i] += avg_CPU_Step[i];

      fprintf(FdCPU, "Step %d, Time: %g, CPUs: %d\n", All.NumCurrentTiStep, All.Time, NTask);
      fprintf(FdCPU,
	      "total         %10.2f  %5.1f%%\n"
	      "treegrav      %10.2f  %5.1f%%\n"
	      "   treebuild  %10.2f  %5.1f%%\n"
	      "   treeupdate %10.2f  %5.1f%%\n"
	      "   treewalk   %10.2f  %5.1f%%\n"
	      "   treecomm   %10.2f  %5.1f%%\n"
	      "   treeimbal  %10.2f  %5.1f%%\n"
#ifdef ADAPTIVE_GRAVSOFT_FORALL
	      "adaptgrav     %10.2f  %5.1f%%\n"
	      "   agsdensity %10.2f  %5.1f%%\n"
	      "   agscomm    %10.2f  %5.1f%%\n"
	      "   agsimbal   %10.2f  %5.1f%%\n"
#endif
	      "pmgrav        %10.2f  %5.1f%%\n"
	      "hydro         %10.2f  %5.1f%%\n"
	      "   density    %10.2f  %5.1f%%\n"
	      "   denscomm   %10.2f  %5.1f%%\n"
	      "   densimbal  %10.2f  %5.1f%%\n"
	      "   hydrofrc   %10.2f  %5.1f%%\n"
	      "   hydcomm    %10.2f  %5.1f%%\n"
	      "   hydmisc    %10.2f  %5.1f%%\n"
	      "   hydnetwork %10.2f  %5.1f%%\n"
	      "   hydimbal   %10.2f  %5.1f%%\n"
	      "   hmaxupdate %10.2f  %5.1f%%\n"
	      "domain        %10.2f  %5.1f%%\n"
	      "potential     %10.2f  %5.1f%%\n"
	      "predict       %10.2f  %5.1f%%\n"
	      "kicks         %10.2f  %5.1f%%\n"
	      "i/o           %10.2f  %5.1f%%\n"
	      "peano         %10.2f  %5.1f%%\n"
	      "sfrcool       %10.2f  %5.1f%%\n"
	      "blackholes    %10.2f  %5.1f%%\n"
	      "fof/subfind   %10.2f  %5.1f%%\n"
          "gas_return    %10.2f  %5.1f%%\n"
          "snII_fb_loop  %10.2f  %5.1f%%\n"
          "hII_fb_loop   %10.2f  %5.1f%%\n"
          "localwindkik  %10.2f  %5.1f%%\n"
          "misc          %10.2f  %5.1f%%\n",
              
    All.CPU_Sum[CPU_ALL], 100.0,
    All.CPU_Sum[CPU_TREEWALK1] + All.CPU_Sum[CPU_TREEWALK2] + All.CPU_Sum[CPU_TREESEND] + All.CPU_Sum[CPU_TREERECV]
              + All.CPU_Sum[CPU_TREEWAIT1] + All.CPU_Sum[CPU_TREEWAIT2] + All.CPU_Sum[CPU_TREEBUILD] + All.CPU_Sum[CPU_TREEUPDATE]
              + All.CPU_Sum[CPU_TREEMISC],
    (All.CPU_Sum[CPU_TREEWALK1] + All.CPU_Sum[CPU_TREEWALK2] + All.CPU_Sum[CPU_TREESEND] + All.CPU_Sum[CPU_TREERECV]
              + All.CPU_Sum[CPU_TREEWAIT1] + All.CPU_Sum[CPU_TREEWAIT2] + All.CPU_Sum[CPU_TREEBUILD] + All.CPU_Sum[CPU_TREEUPDATE] + All.CPU_Sum[CPU_TREEMISC]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREEBUILD], (All.CPU_Sum[CPU_TREEBUILD]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREEUPDATE], (All.CPU_Sum[CPU_TREEUPDATE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREEWALK1] + All.CPU_Sum[CPU_TREEWALK2], (All.CPU_Sum[CPU_TREEWALK1] + All.CPU_Sum[CPU_TREEWALK2]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREESEND] + All.CPU_Sum[CPU_TREERECV], (All.CPU_Sum[CPU_TREESEND] + All.CPU_Sum[CPU_TREERECV]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREEWAIT1] + All.CPU_Sum[CPU_TREEWAIT2], (All.CPU_Sum[CPU_TREEWAIT1] + All.CPU_Sum[CPU_TREEWAIT2]) / All.CPU_Sum[CPU_ALL] * 100,
#ifdef ADAPTIVE_GRAVSOFT_FORALL
    All.CPU_Sum[CPU_AGSDENSCOMPUTE] + All.CPU_Sum[CPU_AGSDENSWAIT] + All.CPU_Sum[CPU_AGSDENSCOMM] + All.CPU_Sum[CPU_AGSDENSMISC],
              (All.CPU_Sum[CPU_AGSDENSCOMPUTE] + All.CPU_Sum[CPU_AGSDENSWAIT] + All.CPU_Sum[CPU_AGSDENSCOMM] + All.CPU_Sum[CPU_AGSDENSMISC]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_AGSDENSCOMPUTE], (All.CPU_Sum[CPU_AGSDENSCOMPUTE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_AGSDENSCOMM], (All.CPU_Sum[CPU_AGSDENSCOMM]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_AGSDENSWAIT], (All.CPU_Sum[CPU_AGSDENSWAIT]) / All.CPU_Sum[CPU_ALL] * 100,
#endif
    All.CPU_Sum[CPU_MESH], (All.CPU_Sum[CPU_MESH]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DENSCOMPUTE] + All.CPU_Sum[CPU_DENSWAIT] + All.CPU_Sum[CPU_DENSCOMM] + All.CPU_Sum[CPU_DENSMISC]
              + All.CPU_Sum[CPU_HYDCOMPUTE] + All.CPU_Sum[CPU_HYDWAIT] + All.CPU_Sum[CPU_TREEHMAXUPDATE]
              + All.CPU_Sum[CPU_HYDCOMM] + All.CPU_Sum[CPU_HYDMISC] + All.CPU_Sum[CPU_HYDNETWORK],
    (All.CPU_Sum[CPU_DENSCOMPUTE] + All.CPU_Sum[CPU_DENSWAIT] + All.CPU_Sum[CPU_DENSCOMM] + All.CPU_Sum[CPU_DENSMISC]
              + All.CPU_Sum[CPU_HYDCOMPUTE] + All.CPU_Sum[CPU_HYDWAIT] + All.CPU_Sum[CPU_TREEHMAXUPDATE]
              + All.CPU_Sum[CPU_HYDCOMM] + All.CPU_Sum[CPU_HYDMISC] + All.CPU_Sum[CPU_HYDNETWORK]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DENSCOMPUTE], (All.CPU_Sum[CPU_DENSCOMPUTE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DENSCOMM], (All.CPU_Sum[CPU_DENSCOMM]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DENSWAIT], (All.CPU_Sum[CPU_DENSWAIT]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HYDCOMPUTE], (All.CPU_Sum[CPU_HYDCOMPUTE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HYDCOMM], (All.CPU_Sum[CPU_HYDCOMM]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HYDMISC], (All.CPU_Sum[CPU_HYDMISC]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HYDNETWORK], (All.CPU_Sum[CPU_HYDNETWORK]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HYDWAIT], (All.CPU_Sum[CPU_HYDWAIT]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TREEHMAXUPDATE], (All.CPU_Sum[CPU_TREEHMAXUPDATE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DOMAIN], (All.CPU_Sum[CPU_DOMAIN]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_POTENTIAL], (All.CPU_Sum[CPU_POTENTIAL]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_DRIFT], (All.CPU_Sum[CPU_DRIFT]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_TIMELINE], (All.CPU_Sum[CPU_TIMELINE]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_SNAPSHOT], (All.CPU_Sum[CPU_SNAPSHOT]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_PEANO], (All.CPU_Sum[CPU_PEANO]) / All.CPU_Sum[CPU_ALL] * 100,
#ifdef COOLING
    All.CPU_Sum[CPU_COOLINGSFR], (All.CPU_Sum[CPU_COOLINGSFR]) / All.CPU_Sum[CPU_ALL] * 100,
#else
    0.,0.,
#endif
    0.,0.,
    0.,0.,
    All.CPU_Sum[CPU_GASRETURN], (All.CPU_Sum[CPU_GASRETURN]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_SNIIHEATING], (All.CPU_Sum[CPU_SNIIHEATING]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_HIIHEATING], (All.CPU_Sum[CPU_HIIHEATING]) / All.CPU_Sum[CPU_ALL] * 100,
    All.CPU_Sum[CPU_LOCALWIND], (All.CPU_Sum[CPU_LOCALWIND]) / All.CPU_Sum[CPU_ALL] * 100,

    All.CPU_Sum[CPU_MISC], (All.CPU_Sum[CPU_MISC]) / All.CPU_Sum[CPU_ALL] * 100);
        
    fprintf(FdCPU, "\n");
    fflush(FdCPU);
    }
}



void put_symbol(double t0, double t1, char c)
{
  int i, j;

  i = (int) (t0 * CPU_STRING_LEN + 0.5);
  j = (int) (t1 * CPU_STRING_LEN);

  if(i < 0)
    i = 0;
  if(j < 0)
    j = 0;
  if(i >= CPU_STRING_LEN)
    i = CPU_STRING_LEN;
  if(j >= CPU_STRING_LEN)
    j = CPU_STRING_LEN;

  while(i <= j)
    CPU_String[i++] = c;

  CPU_String[CPU_STRING_LEN] = 0;
}



/*! This routine first calls a computation of various global
 * quantities of the particle distribution, and then writes some
 * statistics about the energies in the various particle components to
 * the file FdEnergy.
 */
void energy_statistics(void)
{
  compute_global_quantities_of_system();

  if(ThisTask == 0)
    {
      fprintf(FdEnergy,
	      "%g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g",
	      All.Time, SysState.EnergyInt, SysState.EnergyPot, SysState.EnergyKin, SysState.EnergyIntComp[0],
	      SysState.EnergyPotComp[0], SysState.EnergyKinComp[0], SysState.EnergyIntComp[1],
	      SysState.EnergyPotComp[1], SysState.EnergyKinComp[1], SysState.EnergyIntComp[2],
	      SysState.EnergyPotComp[2], SysState.EnergyKinComp[2], SysState.EnergyIntComp[3],
	      SysState.EnergyPotComp[3], SysState.EnergyKinComp[3], SysState.EnergyIntComp[4],
	      SysState.EnergyPotComp[4], SysState.EnergyKinComp[4], SysState.EnergyIntComp[5],
	      SysState.EnergyPotComp[5], SysState.EnergyKinComp[5], SysState.MassComp[0],
	      SysState.MassComp[1], SysState.MassComp[2], SysState.MassComp[3], SysState.MassComp[4],
	      SysState.MassComp[5]);

      fprintf(FdEnergy," \n");
      fflush(FdEnergy);
    }
}




void output_extra_log_messages(void)
{
    
    
    
    if(ThisTask == 0)
    {
    }
}




void check_particles_info(const char *func, const char *file, int linenr)
{
  int i,k,vok=0,pok=0,vsph=0;
  double vv;

  MPI_Barrier(MPI_COMM_WORLD);

  if(ThisTask == 0)
    printf("Checking particle data (function %s in file %s at line %d) ...\n",func,file,linenr);

  for(i = 0; i < NumPart; i++)
    {
      
      for(k = 0; k < 3; k++)
	{
	  if( P[i].Vel[k] > -1e8 && P[i].Vel[k] < 1e8)
	    {
	      vv = sqrt(P[i].Vel[0] * P[i].Vel[0] + P[i].Vel[1] * P[i].Vel[1] + P[i].Vel[2] * P[i].Vel[2]);
	      if(vv > 15000)
		{
		  printf("task=%d: WARNING: Large velocity for particle %d ID %llu v[%d]=%g, renormalizing it !!\n", ThisTask, i,
			 (unsigned long long) P[i].ID,k,vv);
		  fflush(stdout);
                  P[i].Vel[0] = P[i].Vel[0] / vv * 10000;
                  P[i].Vel[1] = P[i].Vel[1] / vv * 10000;
                  P[i].Vel[2] = P[i].Vel[2] / vv * 10000;
		}
	      vok++;
	    }
	  else
	    {
	      printf("task=%d:  strange value in velocity in for particle %d ID %llu , type=%d, mass=%g, v[%d]=%g\n", ThisTask, i,
		     (unsigned long long) P[i].ID,P[i].Type,P[i].Mass,k,P[i].Vel[k]);
		     fflush(stdout);
		     endrun(712401);
	    }

	  if( P[i].Pos[k] > -10000 && P[i].Pos[k] < All.BoxSize + 10000)
	    pok++;
	  else
	    {
	      printf("task=%d:  strange value in position in for particle %d ID %llu x[%d]=%g\n", ThisTask, i,
		     (unsigned long long) P[i].ID,k,P[i].Pos[k]);
		     fflush(stdout);
		     endrun(712402);
	    }
	}

      if(P[i].Type == 0)
        {
          if( (SphP[i].InternalEnergyPred > -1e20 && SphP[i].InternalEnergyPred < 1e20))
            vsph++;
          else
            printf("task=%d: Particle id=%llu, m=%e strange InternalEnergy value in hydro: %e\n",
		   ThisTask,(unsigned long long) P[i].ID,P[i].Mass,SphP[i].InternalEnergyPred);

          if( (SphP[i].DtInternalEnergy > -1e20 && SphP[i].DtInternalEnergy < 1e20))
            vsph++;
          else
            printf("task=%d: Particle id=%llu, m=%e strange DtInternalEnergy value in hydro: %e\n",
		   ThisTask,(unsigned long long) P[i].ID,P[i].Mass,SphP[i].DtInternalEnergy);

          if( (SphP[i].Pressure  > -1e20 && SphP[i].Pressure <1e20))
            vsph++;
          else
            printf("task=%d: Particle id=%llu,m=%e strange Pressure value in hydro: %e\n",
		   ThisTask,(unsigned long long) P[i].ID,P[i].Mass,SphP[i].Pressure);
        }

      if(P[i].Type > 5 || P[i].Type < 0)
	{
	  printf("task=%d:  P[i=%d].Type=%d\n", ThisTask, i, P[i].Type);
	  endrun(712411);
	}

    }

  if(ThisTask == 0)
    printf("Positions and Velocities fine for (%d,%d,%d) of %d cases on task 0...\n",pok,vok,vsph,NumPart*3);

  MPI_Barrier(MPI_COMM_WORLD);
}


