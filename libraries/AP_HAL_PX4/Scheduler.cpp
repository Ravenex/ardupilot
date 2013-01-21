/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_PX4

#include "AP_HAL_PX4.h"
#include "Scheduler.h"

#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <drivers/drv_hrt.h>
#include <nuttx/arch.h>
#include <systemlib/systemlib.h>
#include <poll.h>

using namespace PX4;

extern const AP_HAL::HAL& hal;

extern bool _px4_thread_should_exit;

PX4Scheduler::PX4Scheduler()
{}

void PX4Scheduler::init(void *unused) 
{
    _sketch_start_time = hrt_absolute_time();

    // setup the timer thread - this will call tasks at 1kHz
	pthread_attr_t thread_attr;
	pthread_attr_init(&thread_attr);
	pthread_attr_setstacksize(&thread_attr, 2048);

    // the timer thread needs a higher priority than the main code, so
    // it runs as soon as its poll() delay returns
	struct sched_param param;
	param.sched_priority = SCHED_PRIORITY_DEFAULT + 1;
	(void)pthread_attr_setschedparam(&thread_attr, &param);

	pthread_create(&_thread, &thread_attr, (pthread_startroutine_t)&PX4::PX4Scheduler::_timer_thread, this);
}

uint32_t PX4Scheduler::micros() 
{
    return (uint32_t)(hrt_absolute_time() - _sketch_start_time);
}

uint32_t PX4Scheduler::millis() 
{
    return hrt_absolute_time() / 1000;
}

void PX4Scheduler::delay_microseconds(uint16_t usec) 
{
	uint32_t start = micros();
	while (micros() - start < usec) {
		up_udelay(usec - (micros() - start));
	}
}

void PX4Scheduler::delay(uint16_t ms)
{
	uint64_t start = hrt_absolute_time();
    
    while ((hrt_absolute_time() - start)/1000 < ms && 
           !_px4_thread_should_exit) {
        // this yields the CPU to other apps
        poll(NULL, 0, 1);
        if (_min_delay_cb_ms <= ms) {
            if (_delay_cb) {
                _delay_cb();
            }
        }
    }
    if (_px4_thread_should_exit) {
        exit(1);
    }
}

void PX4Scheduler::register_delay_callback(AP_HAL::Proc proc,
                                            uint16_t min_time_ms) 
{
    _delay_cb = proc;
    _min_delay_cb_ms = min_time_ms;
}

void PX4Scheduler::register_timer_process(AP_HAL::TimedProc proc) 
{
    for (uint8_t i = 0; i < _num_timer_procs; i++) {
        if (_timer_proc[i] == proc) {
            return;
        }
    }

    if (_num_timer_procs < PX4_SCHEDULER_MAX_TIMER_PROCS) {
        _timer_proc[_num_timer_procs] = proc;
        _num_timer_procs++;
    } else {
        hal.console->printf("Out of timer processes\n");
    }
}

void PX4Scheduler::register_timer_failsafe(AP_HAL::TimedProc failsafe, uint32_t period_us) 
{
    _failsafe = failsafe;
}

void PX4Scheduler::suspend_timer_procs() 
{
    _timer_suspended = true;
}

void PX4Scheduler::resume_timer_procs() 
{
    _timer_suspended = false;
    if (_timer_event_missed == true) {
        _run_timers(false);
        _timer_event_missed = false;
    }
}

void PX4Scheduler::reboot() 
{
	up_systemreset();
}

void PX4Scheduler::_run_timers(bool called_from_timer_thread)
{
    uint32_t tnow = micros();
    if (_in_timer_proc) {
        return;
    }
    _in_timer_proc = true;

    if (!_timer_suspended) {
        // now call the timer based drivers
        for (int i = 0; i < _num_timer_procs; i++) {
            if (_timer_proc[i] != NULL) {
                _timer_proc[i](tnow);
            }
        }
    } else if (called_from_timer_thread) {
        _timer_event_missed = true;
    }

    // and the failsafe, if one is setup
    if (_failsafe != NULL) {
        _failsafe(tnow);
    }

    _in_timer_proc = false;
}

void *PX4Scheduler::_timer_thread(void)
{
    while (!_px4_thread_should_exit) {
        // run timers at 1kHz
        poll(NULL, 0, 1);
        _run_timers(true);
    }
    return NULL;
}

void PX4Scheduler::panic(const prog_char_t *errormsg) 
{
    write(1, errormsg, strlen(errormsg));
    hal.scheduler->delay_microseconds(10000);
    _px4_thread_should_exit = true;
    exit(1);
}

bool PX4Scheduler::in_timerprocess() {
    return _in_timer_proc;
}

bool PX4Scheduler::system_initializing() {
    return !_initialized;
}

void PX4Scheduler::system_initialized() {
    if (_initialized) {
        panic(PSTR("PANIC: scheduler::system_initialized called"
                   "more than once"));
    }
    _initialized = true;
}

#endif
