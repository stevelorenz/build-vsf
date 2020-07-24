/*
 * scaling_helpers_user.h
 */

#ifndef SCALING_HELPERS_USER_H
#define SCALING_HELPERS_USER_H

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <rte_power.h>

#include <ffpp/bpf_defines_user.h>
#include <ffpp/scaling_defines_user.h>
#include <ffpp/general_helpers_user.h>
#include <ffpp/global_stats_user.h>

double g_csv_pps[TOTAL_VALS];
double g_csv_ts[TOTAL_VALS];
double g_csv_cpu_util[TOTAL_VALS];
unsigned int g_csv_freq[TOTAL_VALS];
unsigned int g_csv_num_val;
int g_csv_num_round;
bool g_csv_saved_stream;
double cur_time;

/**
 * Obtain number of p-states and respective frequencies of the given lcore
 * 
 * @param lcore: Core for that the info is needed
 * @param f: Struct to store the information
 * @param debug: Prints the obtained information if true
 */
void get_frequency_info(int lcore, struct freq_info *f, bool debug);

/**
 * Reads the current frequency of the given core from /proc/cpuinfo
 * 
 * @param lcore: core id for the core of interest
 * 
 * @return
 *  - Frequency for the given core
 */
double get_cpu_frequency(int lcore);

/**
 * Calculate simple moving averager over the last NUM_READINGS_SMA values
 * 
 * @param m: struct of the current measurement status and values
 */
void calc_sma(struct measurement *m);

/**
 * Calculate weighted moving averager over the last NUM_READINGS_SMA values
 * 
 * @param m: struct of the current measurement status and values
 */
void calc_wma(struct measurement *m);

/**
 * Calculates the time between two map readings
 * 
 * @param r: struct of the current reading
 * @param p: struct of the previous reading
 * 
 * @return
 *  - user space time between current and previous reading
 */
double calc_period(struct record *r, struct record *p);

/**
 * Sends CPU to the given p-state
 * 
 * @param f: struct with frequency information of the CPU
 * @param pstate: pstate the CPU shall enter
 */
void set_pstate(struct freq_info *f, struct scaling_info *si);

/**
 * Enables Turbo Boost for the CPU
 */
void set_turbo();

/**
 * Calculates the p-state that is required for the current inter-arrival
 * time and CPU utilization threshold
 * 
 * @param m: struct of the current measurement status and values
 * @param f: struct with frequency information of the CPU
 * 
 * @return
 *  - p-state index
 */
void calc_pstate(struct measurement *m, struct freq_info *f,
		 struct scaling_info *si);

/**
 * Check if the traffic follows certain trends
 * 
 * @param m: struct of the current measurement status and values
 * @param si: struct for scaling informations
 */
void check_traffic_trends(struct measurement *m, struct scaling_info *si);

/**
 * Checks the current CPU utilization and increnebts the 
 * respective counter
 * 
 * @param m: struct of the current measurement status and values
 * @param f: struct with frequency information of the CPU
 * @param si: struct with flags and counters for scaling decisions
 */
void check_frequency_scaling(struct measurement *m, struct freq_info *f,
			     struct scaling_info *si);

/**
 * Brings the frequency back-up to the one of the previous stream after
 * an ISG was detected
 * 
 * @param lss: struct with settings of last stream
 * @param f: struct with frequency information of the CPU
 * @param si: struct with flags and counters for scaling decisions
 */
void restore_last_stream_settings(struct last_stream_settings *lss,
				  struct freq_info *f, struct scaling_info *si);

/**
 * Get some statistics one the received traffic
 * 
 * @param m: struct of the current measurement status and values
 * @param r: struct of the current reading
 * @param p: struct of the previous reading
 * @param t_s: struct of the traffic stats
 */
void calc_traffic_stats(struct measurement *m, struct record *r,
			struct record *p, struct traffic_stats *t_s,
			struct scaling_info *si);

/**
 * Calculates the CPU utilization according to the recent traffic
 * 
 * @param m: struct of the current measurement status and values
 * @param f: struct with frequency information of the CPU
 */
void get_cpu_utilization(struct measurement *m, struct freq_info *f);

#endif /* !SCALING_HELPERS_USER_H */
