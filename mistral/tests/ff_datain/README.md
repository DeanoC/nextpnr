# Flip-flop data-source check

Cyclone V flip-flops select either the associated combinational output or the
half-ALM E/F input. They do not have a hard constant selector. The fixture
co-packs active LUTs in the same half as three flip-flops and checks explicit
zero, explicit one, and routed-signal data inputs. Explicit constants must use
the E/F input and a packer constant source rather than silently selecting the
active combinational output. An omitted primitive DATAIN remains outside this
contract because it has no defined logical value.
