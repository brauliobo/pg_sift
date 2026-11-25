package SiftCompat;
use Cwd ();
use File::Basename ();
require(Cwd::abs_path(File::Basename::dirname(__FILE__)) . "/lib/SiftCompat.pm");
1;
