#include "Main/Menu/menu_label_cell_rows.h"

const MultiSelectMemberCycleStep gMultiSelectMemberCycleSteps[4] = {{0, 0x10}, {1, 6}, {2, 6}, {1, 6}};
const u16 gMultiSelectMemberCountCellIndices[8] = {4, 5, 6, 7, 8, 9, 0xa, 0xb};
const LabelMessageRow gMultiSelectMemberMainLabelRows[2] = {{0, 0xbe, 0xd, 0, 5}, {1, 0xbe, 0xd, 1, 5}};
const LabelMessageRow gMultiSelectMemberSubLabelRowsStorage_[9] = {
    {0, 0x82, 0xd, 0, -1}, {1, 0x82, 0xd, 2, -1}, {2, 0x82, 0xd, 4, -1}, {3, 0x82, 0xd, 6, -1}, {4, 0x82, 0xd, 8, -1},
    {5, 0x82, 0xd, 10, -1}, {6, 0x82, 0xd, 12, -1}, {7, 0x82, 0xd, 14, -1}, {8, 0x78, 0xd, 17, 6}};
__asm__(".globl gMultiSelectMemberSubLabelRows\n.set gMultiSelectMemberSubLabelRows, gMultiSelectMemberSubLabelRowsStorage_");
