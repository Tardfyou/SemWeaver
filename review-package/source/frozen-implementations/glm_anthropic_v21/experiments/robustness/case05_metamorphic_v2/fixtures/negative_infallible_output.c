struct firmware {
    int ready;
};

extern void consume(const struct firmware *fw);

static int cached_firmware(const struct firmware **fw, const char *name,
                           void *device)
{
    static const struct firmware cached = { 1 };
    (void)name;
    (void)device;
    *fw = &cached;
    return 0;
}

int load_device(void *device)
{
    const struct firmware *fw = 0;
    int ret = cached_firmware(&fw, "device.bin", device);
    if (fw)
        consume(fw);
    return ret;
}
