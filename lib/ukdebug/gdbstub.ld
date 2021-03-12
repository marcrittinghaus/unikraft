SECTIONS
{
	/* GDB stub */
	__gdb_target_xml_start = .;
	.gdb.target.xml :
	{
		KEEP (*(.gdb.target.xml))
	}
	__gdb_target_xml_end = .;
}
INSERT AFTER .text;