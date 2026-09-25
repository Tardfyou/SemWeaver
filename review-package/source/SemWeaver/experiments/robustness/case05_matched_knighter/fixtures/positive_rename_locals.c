struct firmware;
extern int request_firmware(const struct firmware **fw, const char *name, void *device);
extern void consume(const struct firmware *fw);

int load_device(void *device)
{
    const struct firmware *image = 0;
    int status = request_firmware(&image, "device.bin", device);
    if (image)
        consume(image);
    return status;
}
