struct firmware;
extern int request_firmware(const struct firmware **fw, const char *name, void *device);
extern void consume(const struct firmware *fw);
extern int device_ready(void);

int load_device(void *device)
{
    const struct firmware *fw = 0;
    int ret = request_firmware(&fw, "device.bin", device);
    if (fw && device_ready())
        consume(fw);
    return ret;
}
