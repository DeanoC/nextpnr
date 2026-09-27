#ifndef HEAP_PIN_OFFSET_H
#define HEAP_PIN_OFFSET_H
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN
inline int heap_pin_position(int cell_position, int offset) { return cell_position + offset; }
template <typename System>
void heap_stamp_port_term(System &es, int row, int column, bool solved, int cell_position, int offset, double weight)
{
    if (solved) es.add_coeff(row,column,weight);
    else es.add_rhs(row,-cell_position*weight);
    if (offset != 0) es.add_rhs(row,-offset*weight);
}
NEXTPNR_NAMESPACE_END
#endif
