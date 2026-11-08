import evdev
from evdev import InputDevice, ecodes

MODULO = 2 ** 16
COUNTS_PER_0p02 = 256
T_UNIT = 0.02
ACCEL = (ecodes.ABS_X, ecodes.ABS_Y, ecodes.ABS_Z)
GYRO = (ecodes.ABS_RX, ecodes.ABS_RY, ecodes.ABS_RZ)


def find_imu_device():
    for path in evdev.list_devices():
        dev = InputDevice(path)
        if "IMU" in dev.name:
            return dev
        dev.close()
    raise RuntimeError("No IMU evdev device found")


def read_samples(dev):
    accel = [dev.absinfo(code).value for code in ACCEL]
    gyro = [dev.absinfo(code).value for code in GYRO]
    counter = 0
    last_counter = None

    for event in dev.read_loop():
        if event.type == ecodes.EV_ABS:
            if event.code in ACCEL:
                accel[ACCEL.index(event.code)] = event.value
            elif event.code in GYRO:
                gyro[GYRO.index(event.code)] = event.value
        elif event.type == ecodes.EV_MSC and event.code == ecodes.MSC_SERIAL:
            counter = event.value
        elif event.type == ecodes.EV_SYN:
            dt = None
            if last_counter is not None:
                delta = (counter - last_counter) % MODULO
                dt = (delta / COUNTS_PER_0p02) * T_UNIT
            last_counter = counter
            yield counter, dt, tuple(accel), tuple(gyro)
