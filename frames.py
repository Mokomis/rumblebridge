"""Razer haptic stream frames and Protocol 2.5 reports, as Kishi V3 Pro firmware 2.0.0.0 takes them."""

def stream_frame(low, high, duration=30, low_freq=0, high_freq=0):
    """Two-motor rumble (0-255 each) as a 15-byte stream frame: 7-bit duration, then per channel one
    band of four (6-bit amplitude, 7-bit frequency) pairs and no transients, packed MSB first."""
    bits = []
    def add(value, count):
        bits.extend((value >> i) & 1 for i in range(count - 1, -1, -1))
    add(duration, 7)
    for amp, freq in ((low, low_freq), (high, high_freq)):
        add(1, 1)
        for _ in range(4):
            add(int(amp / 255 * 63), 6)
            add(freq, 7)
        add(0, 1)  # no further bands
        add(0, 1)  # no transients
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 1 << (7 - i % 8)
    return bytes(out)

def report25(command_class, command_id, data=b"", transaction=0x1F):
    """A 90-byte Razer Protocol 2.5 feature report."""
    r = bytearray(90)
    r[1] = transaction
    r[5] = len(data)
    r[6] = command_class
    r[7] = command_id
    r[8:8 + len(data)] = data
    crc = 0
    for b in r[2:88]:
        crc ^= b
    r[88] = crc
    return bytes(r)

if __name__ == "__main__":
    print(stream_frame(255, 255).hex())
