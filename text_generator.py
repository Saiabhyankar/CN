import os
import sys
import random
import string

def generate_text_file(filename, size_mb):
    """Generate a random text file of the specified size in MB."""
    size_bytes = size_mb * 1024 * 1024
    chunk_size = 1024 * 1024  # 1 MB at a time

    chars = string.ascii_letters + string.digits + " \n"

    with open(filename, "w") as f:
        bytes_written = 0
        while bytes_written < size_bytes:
            chunk = ''.join(random.choices(chars, k=min(chunk_size, size_bytes - bytes_written)))
            f.write(chunk)
            bytes_written += len(chunk)

    print(f"✅ File '{filename}' created with size ~{size_mb} MB ({bytes_written} bytes)")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python generate_text_file.py <output_file> <size_in_MB>")
        sys.exit(1)

    filename = sys.argv[1]
    size_mb = float(sys.argv[2])
    generate_text_file(filename, size_mb)
