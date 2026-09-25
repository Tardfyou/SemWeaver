struct firmware;
extern int request_firmware(const struct firmware **fw, const char *name, void *device);
extern void consume(const struct firmware *fw);

static int acquire(const struct firmware **out, void *device)
{
    return request_firmware(out, "device.bin", device);
}

int load_device(void *device)
{
    const struct firmware *fw = 0;
    int ret = acquire(&fw, device);
    if (fw)
        consume(fw);
    return ret;
}
