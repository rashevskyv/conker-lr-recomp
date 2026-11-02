"""RECOMP_LANGUAGE -> XC_LANGUAGE (xbox_eeprom.h, xbox_eeprom_language).

The console's language setting picks the text bank a title loads (Conker:
aid/text/<Language>). A number 1-9 or a name in any case; anything else is
English (1), as the kernel answered before.

    python3 -m pytest tools/recomp/test_eeprom_language.py
"""
import os
import subprocess
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
KERNEL = os.path.join(ROOT, 'src', 'kernel')

PROGRAM = r'''
#include <stdio.h>
#include "xbox_eeprom.h"
int main(void)
{
    const char *in[] = { 0, "", "english", "French", "GERMAN", "spanish", "italian",
                         "japanese", "korean", "chinese", "portuguese", "4", "9", "0",
                         "10", "fr", "frenchx", "ukrainian" };
    unsigned i;
    for (i = 0; i < sizeof in / sizeof in[0]; i++)
        printf("%u ", xbox_eeprom_language(in[i]));
    printf("\n");
    return 0;
}
'''


def test_eeprom_language():
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 't.c')
        exe = os.path.join(d, 't.exe')
        with open(src, 'w') as f:
            f.write(PROGRAM)
        subprocess.check_call(['cc', '-std=c99', '-Wall', '-Werror', '-I', KERNEL, src, '-o', exe])
        out = subprocess.check_output([exe]).decode().split()
    assert out == ['1', '1', '1', '4', '3', '5', '6', '2', '7', '8', '9', '4', '9', '1',
                   '1', '1', '1', '1']
