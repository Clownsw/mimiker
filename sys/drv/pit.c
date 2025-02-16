/* Programmable Interval Timer (PIT) driver for Intel 8254 */
#include <sys/klog.h>
#include <dev/i8253reg.h>
#include <dev/isareg.h>
#include <sys/bus.h>
#include <sys/timer.h>
#include <sys/devclass.h>

typedef struct pit_state {
  resource_t *regs;
  resource_t *irq_res;
  timer_t timer;
  bool noticed_overflow; /* noticed and handled the counter overflow */
  uint16_t period_cntr;  /* number of counter ticks in a period */
  /* values since last counter read */
  uint16_t prev_cntr16; /* number of counter ticks */
  /* values since initialization */
  uint32_t cntr_modulo; /* number of counter ticks modulo TIMER_FREQ*/
  uint64_t sec;         /* seconds */
} pit_state_t;

#define inb(addr) bus_read_1(pit->regs, (addr))
#define outb(addr, val) bus_write_1(pit->regs, (addr), (val))

static inline void pit_set_frequency(pit_state_t *pit) {
  outb(TIMER_MODE, TIMER_SEL0 | TIMER_16BIT | TIMER_RATEGEN);
  outb(TIMER_CNTR0, pit->period_cntr & 0xff);
  outb(TIMER_CNTR0, pit->period_cntr >> 8);
}

static inline uint16_t pit_get_counter(pit_state_t *pit) {
  uint16_t count = 0;
  outb(TIMER_MODE, TIMER_SEL0 | TIMER_LATCH);
  count |= inb(TIMER_CNTR0);
  count |= inb(TIMER_CNTR0) << 8;

  /* PIT counter counts from n to 1, we make it ascending from 0 to n-1*/
  return pit->period_cntr - count;
}

static inline void pit_incr_cntr(pit_state_t *pit, uint16_t ticks) {
  pit->cntr_modulo += ticks;
  if (pit->cntr_modulo >= TIMER_FREQ) {
    pit->cntr_modulo -= TIMER_FREQ;
    pit->sec++;
  }
}

static void pit_update_time(pit_state_t *pit) {
  assert(intr_disabled());
  uint64_t last_sec = pit->sec;
  uint32_t last_cntr = pit->cntr_modulo;
  uint16_t now_cntr16 = pit_get_counter(pit);
  uint16_t ticks_passed = now_cntr16 - pit->prev_cntr16;

  if (pit->prev_cntr16 > now_cntr16) {
    pit->noticed_overflow = true;
    ticks_passed += pit->period_cntr;
  }

  /* We want to keep the last read counter value to detect possible future
   * overflows of our counter */
  pit->prev_cntr16 = now_cntr16;

  pit_incr_cntr(pit, ticks_passed);
  assert(last_sec < pit->sec ||
         (last_sec == pit->sec && last_cntr < pit->cntr_modulo));
  assert(pit->cntr_modulo < TIMER_FREQ);
}

static intr_filter_t pit_intr(void *data) {
  pit_state_t *pit = data;

  /* XXX: It's still possible for periods to be lost.
   * For example disabling interrupts for the whole period
   * without calling pit_gettime will lose period_cntr.
   * It is also possible that time suddenly jumps by period_cntr
   * due to the fact that pit_update_time() can't detect an overflow if
   * the current counter value is greater than the previous one, while
   * pit_intr() can thanks to the noticed_overflow flag. */
  pit_update_time(pit);
  if (!pit->noticed_overflow)
    pit_incr_cntr(pit, pit->period_cntr);
  tm_trigger(&pit->timer);
  /* It is set here to let us know in the next interrupt if we already
   * considered the overflow */
  pit->noticed_overflow = false;
  return IF_FILTERED;
}

static device_t *device_of(timer_t *tm) {
  return tm->tm_priv;
}

static int pit_timer_start(timer_t *tm, unsigned flags, const bintime_t start,
                           const bintime_t period) {
  assert(flags & TMF_PERIODIC);
  assert(!(flags & TMF_ONESHOT));

  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;

  uint64_t counter = bintime_mul(period, TIMER_FREQ).sec;
  /* Maximal counter value which we can store in pit timer */
  assert(counter <= 0xFFFF);

  pit->sec = 0;
  pit->cntr_modulo = 0;
  pit->prev_cntr16 = 0;
  pit->period_cntr = counter;
  pit->noticed_overflow = false;

  pit_set_frequency(pit);

  pic_setup_intr(dev, pit->irq_res, pit_intr, NULL, pit, "i8254 timer");
  return 0;
}

static int pit_timer_stop(timer_t *tm) {
  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;
  pic_teardown_intr(dev, pit->irq_res);
  return 0;
}

static bintime_t pit_timer_gettime(timer_t *tm) {
  device_t *dev = device_of(tm);
  pit_state_t *pit = dev->state;
  uint64_t sec;
  uint32_t cntr_modulo;

  WITH_INTR_DISABLED {
    pit_update_time(pit);
    sec = pit->sec;
    cntr_modulo = pit->cntr_modulo;
  }

  bintime_t bt = bintime_mul(tm->tm_min_period, cntr_modulo);
  assert(bt.sec == 0);
  bt.sec = sec;

  return bt;
}

static int pit_attach(device_t *dev) {
  pit_state_t *pit = dev->state;
  int err = 0;

  pit->regs = device_take_ioports(dev, 0);
  assert(pit->regs != NULL);

  if ((err = bus_map_resource(dev, pit->regs)))
    return err;

  pit->irq_res = device_take_irq(dev, 0);

  pit->timer = (timer_t){
    .tm_name = "i8254",
    .tm_flags = TMF_PERIODIC,
    .tm_quality = 100,
    .tm_frequency = TIMER_FREQ,
    .tm_min_period = HZ2BT(TIMER_FREQ),
    .tm_max_period = bintime_mul(HZ2BT(TIMER_FREQ), 65536),
    .tm_start = pit_timer_start,
    .tm_stop = pit_timer_stop,
    .tm_gettime = pit_timer_gettime,
    .tm_priv = dev,
  };

  tm_register(&pit->timer);

  return 0;
}

static int pit_probe(device_t *dev) {
  return dev->unit == 3; /* XXX: unit 3 assigned by gt_pci */
}

static driver_t pit_driver = {
  .desc = "i8254 PIT driver",
  .size = sizeof(pit_state_t),
  .pass = FIRST_PASS,
  .attach = pit_attach,
  .probe = pit_probe,
};

DEVCLASS_ENTRY(isa, pit_driver);
