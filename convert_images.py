from PIL import Image, ImageChops, ImageDraw

def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def image_to_c_array(img, var_name):
    width, height = img.size
    out = f"const int {var_name}_width = {width};\n"
    out += f"const int {var_name}_height = {height};\n"
    out += f"const uint16_t {var_name}[{width * height}] PROGMEM = {{\n"
    pixels = list(img.getdata())
    for i in range(0, len(pixels), width):
        row = pixels[i:i+width]
        out += ",".join(f"0x{rgb888_to_rgb565(r, g, b):04X}" for (r, g, b) in row) + ",\n"
    out += "};\n"
    return out

if __name__ == '__main__':
    print("Use images.h directly (contains authentic Lelit Mara X PL62X and zero-white-border Lelit Red Capsule Logo).")
