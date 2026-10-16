/* The external consumer keeps the complete volatile frame live at -O1. */
extern void consume_frame(volatile unsigned char *);

void moq_frame_probe(void)
{
    volatile unsigned char frame[4096];
    frame[0] = 0;
    consume_frame(frame);
}
