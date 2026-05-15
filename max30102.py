class MAX30102:
    def __init__(self, i2c, address=0x22):
        self.i2c = i2c
        self.addr = address

    def read_heart_rate(self):
        # The custom chip increments every read
        data = self.i2c.readfrom(self.addr, 1)
        value = data[0]

        # Convert to BPM range 50–160
        bpm = 50 + (value % 110)
        return bpm
