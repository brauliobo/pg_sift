#!/usr/bin/env perl
# Extracts every GUC the C sources register, with its file:line, type, default, bounds, context, the preprocessor
# guards around the call and the files that read its C variable. One tab separated row per GUC:
#
#   name file line kind var boot min max context flags guards users
#
# usage: audit-gucs.pl SRC_DIR [SRC_DIR...]      (SRC_DIR is read only)
#
# Registrations are found as DefineCustom{Bool,Int,Real,String,Enum}Variable(...) and sift_host_enum_option(...).
# A file whose DefineCustom* call takes a computed name (a table of settings) is read as the table: every row
# {"sift.name", "description", &variable, default, minimum} is one setting.
use strict;
use warnings;

die "usage: $0 SRC_DIR...\n" unless @ARGV;

my @files = split /\0/, qx{find @{[ join ' ', map { quotemeta } @ARGV ]} -type f \\( -name '*.c' -o -name '*.h' \\) -print0 | sort -z};
my (%text, @rows);

sub strip_comments
{
	my ($s) = @_;
	$s =~ s{/\*.*?\*/}{ my $c = $&; $c =~ s/[^\n]/ /g; $c }gse;
	$s =~ s{//[^\n]*}{ ' ' x length($&) }ge;
	return $s;
}

# guards[line] = list of conditions active on that line (1-based)
sub line_guards
{
	my ($lines) = @_;
	my (@stack, @out);
	for my $i (0 .. $#$lines)
	{
		my $l = $lines->[$i];
		if    ($l =~ /^\s*#\s*ifdef\s+(\w+)/)  { push @stack, $1 }
		elsif ($l =~ /^\s*#\s*ifndef\s+(\w+)/) { push @stack, "!$1" }
		elsif ($l =~ /^\s*#\s*if\s+(.*?)\s*$/) { (my $c = $1) =~ s/\s+/ /g; push @stack, $c }
		elsif ($l =~ /^\s*#\s*elif\s+(.*?)\s*$/) { my $p = pop @stack; push @stack, "!($p) && $1" }
		elsif ($l =~ /^\s*#\s*else\b/) { my $p = pop @stack; push @stack, $p =~ /^!([A-Za-z_]\w*)$/ ? $1 : "!($p)" }
		elsif ($l =~ /^\s*#\s*endif\b/) { pop @stack }
		$out[$i + 1] = join(' && ', @stack);
	}
	return \@out;
}

sub split_args
{
	my ($s, $open) = @_;
	my @args;
	my ($cur, $depth, $i) = ('', 0, $open + 1);
	while ($i < length $s)
	{
		my $c = substr($s, $i, 1);
		if ($c eq '"')
		{
			my $j = $i + 1;
			$j += (substr($s, $j, 1) eq '\\') ? 2 : 1 while $j < length($s) && substr($s, $j, 1) ne '"';
			die "unterminated string literal near offset $i\n" if $j >= length $s;
			$cur .= substr($s, $i, $j - $i + 1);
			$i = $j + 1;
			next;
		}
		if ($c =~ /[(\[{]/) { $depth++ }
		elsif ($c =~ /[)\]}]/)
		{
			last if $depth == 0;
			$depth--;
		}
		elsif ($c eq ',' && $depth == 0) { push @args, $cur; $cur = ''; $i++; next }
		$cur .= $c;
		$i++;
	}
	push @args, $cur;
	s/\s+/ /g, s/^ //, s/ $// for @args;
	return @args;
}

sub unq { my ($s) = @_; return $s =~ /^"(.*)"$/ ? $1 : undef }

for my $f (@files)
{
	open my $fh, '<', $f or die "$f: $!\n";
	local $/;
	my $raw = <$fh>;
	close $fh;
	$text{$f} = strip_comments($raw);
}

for my $f (@files)
{
	my $t = $text{$f};
	next unless $t =~ /DefineCustom\w+Variable|sift_host_enum_option/;
	my @lines  = split /\n/, $t, -1;
	my $guards = line_guards(\@lines);
	my $dynamic = 0;
	my @starts = ([0]);
	push @starts, [ $starts[-1][0] + length($_) + 1 ] for @lines;
	my $line_of = sub { my ($o) = @_; my ($lo, $hi) = (0, $#starts); while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($starts[$m][0] <= $o) { $lo = $m } else { $hi = $m - 1 } } return $lo + 1 };

	while ($t =~ /\b(DefineCustom(Bool|Int|Real|String|Enum)Variable|sift_host_enum_option)\s*\(/g)
	{
		my ($fn, $type, $open, $at) = ($1, $2, $+[0] - 1, $-[0]);
		my $ln = $line_of->($at);
		next if $lines[ $ln - 1 ] =~ /^\s*#\s*define\b/;
		my @a = split_args($t, $open);
		my $name = unq($a[0]);
		if (!defined $name) { $dynamic = 1; next }
		my ($kind, $var, $boot, $min, $max, $ctx, $flags) = (lc($type // 'enum'));
		$var = $a[3];
		$var =~ s/^&//;
		$boot = $a[4];
		if    ($kind eq 'bool' || $kind eq 'string') { ($ctx, $flags) = @a[ 5, 6 ] }
		elsif ($kind eq 'int' || $kind eq 'real')    { ($min, $max, $ctx, $flags) = @a[ 5 .. 8 ] }
		elsif ($fn eq 'sift_host_enum_option')       { ($ctx, $flags) = ('PGC_USERSET', '0') }
		else                                         { ($ctx, $flags) = @a[ 6, 7 ] }
		push @rows, { name => $name, file => $f, line => $ln, kind => $kind, var => $var, boot => $boot,
			min => $min // '', max => $max // '', ctx => $ctx // '', flags => $flags // '', guards => $guards->[$ln] // '' };
	}

	next unless $dynamic;
	while ($t =~ /\{\s*"(sift\.[\w.]+)"\s*,\s*"[^"]*"\s*,\s*&(\w+)\s*,\s*([-\w.]+)\s*,\s*([-\w.]+)\s*\}/g)
	{
		my ($name, $var, $boot, $min, $at) = ($1, $2, $3, $4, $-[0]);
		my $ln = $line_of->($at);
		push @rows, { name => $name, file => $f, line => $ln, kind => 'int', var => $var, boot => $boot, min => $min,
			max => 'INT_MAX', ctx => 'PGC_USERSET', flags => 'GUC_NOT_IN_SAMPLE', guards => $guards->[$ln] // '' };
	}
}

my %want = map { $_->{var} => 1 } @rows;
my %users;
for my $f (@files)
{
	my %seen;
	$seen{$_} = 1 for grep { $want{$_} } $text{$f} =~ /\b([A-Za-z_]\w*)\b/g;
	push @{ $users{$_} }, $f for keys %seen;
}

for my $r (sort { $a->{name} cmp $b->{name} || $a->{file} cmp $b->{file} || $a->{line} <=> $b->{line} } @rows)
{
	my @u = sort grep { $_ ne $r->{file} } @{ $users{ $r->{var} } // [] };
	print join("\t", @$r{qw(name file line kind var boot min max ctx flags)}, $r->{guards}, join(',', @u)), "\n";
}
