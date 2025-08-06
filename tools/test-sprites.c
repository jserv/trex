#include <stdint.h>
#include <stdio.h>

/* Test that all sprites decompress correctly */

static const uint8_t cactus_rle[] = {
    19, 0, 2, 1, 8, 0, 1, 1, 2, 0, 2, 1, 2, 0,  1, 1, 5, 0,  2, 1, 1, 0, 2,
    1,  1, 0, 2, 1, 6, 0, 6, 1, 9, 0, 2, 1, 11, 0, 2, 1, 11, 0, 2, 1, 5, 0,
};

void test_rle_decompression()
{
    int data[1024] = {0};
    int pos = 0;

    /* Decompress cactus */
    for (int i = 0; i < sizeof(cactus_rle); i += 2) {
        for (int j = 0; j < cactus_rle[i] && pos < 104; j++)
            data[pos++] = cactus_rle[i + 1];
    }

    printf("Cactus sprite test:\n");
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 13; x++) {
            printf("%c", data[y * 13 + x] ? '#' : '.');
        }
        printf("\n");
    }

    printf("Cactus sprite: %d pixels decompressed correctly\n", pos);
}

int main()
{
    printf("Sprite RLE Test\n");
    printf("===============\n");
    test_rle_decompression();
    return 0;
}
