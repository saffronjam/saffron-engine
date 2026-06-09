local Mover = {}
function Mover.on_update(self, dt)
  local p = self.entity:get_position()
  self.entity:set_position(p.x + dt, p.y, p.z)
end
return Mover
