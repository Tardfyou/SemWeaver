struct firmware;
extern int request_firmware(const struct firmware **fw, const char *name, void *device);

int load_device(void *device)
{
    const struct firmware *fw = 0;
    int ret = request_firmware(&fw, "device.bin", device);
    return ret;
}
