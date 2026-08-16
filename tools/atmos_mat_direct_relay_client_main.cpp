#include "tools/atmos_mat_direct_relay_cli.h"
int main(int argc, char **argv) { const auto options=atmos_mat_relay_cli::parse(argc,const_cast<const char *const *>(argv)); return !options || options->mode!=atmos_mat_relay_cli::role::client ? 2 : atmos_mat_relay_cli::run(*options); }
