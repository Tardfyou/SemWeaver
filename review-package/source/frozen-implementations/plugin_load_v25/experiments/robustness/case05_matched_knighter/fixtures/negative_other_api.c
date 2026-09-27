struct firmware;
extern int cached_firmware(const struct firmware **fw, const char *name, void *device);
extern void consume(const struct firmware *fw);

int load_device(void *device)
{
    const struct firmware *fw = 0;
    int ret = cached_firmware(&fw, "device.bin", device);
    if (fw)
        consume(fw);
    return ret;
}
