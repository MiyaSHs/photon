-- the version values addons read (PAC3's luadata keys its bytecode table on jit.version_num)
return table.concat({ tostring(_VERSION), tostring(jit.version), tostring(jit.version_num), tostring(jit.os), tostring(jit.arch),
  tostring(jit.status()) }, "\n")
