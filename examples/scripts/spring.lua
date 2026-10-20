return {
    step_hz = 120,
    parameters = {
        target = { type = "number", default = 600 },
        stiffness = { type = "number", default = 80, min = 0, max = 1000 },
        damping = { type = "number", default = 12, min = 0, max = 100 },
    },
    initialize = function(ctx)
        ctx.state = { x = ctx.entity.transform.x, velocity = 0 }
    end,
    step = function(ctx)
        local state, params, dt = ctx.state, ctx.params, ctx.time.dt
        local acceleration = (params.target - state.x) * params.stiffness
            - state.velocity * params.damping
        state.velocity = state.velocity + acceleration * dt
        state.x = state.x + state.velocity * dt
    end,
    sample = function(ctx)
        ctx.entity.transform.x = ctx.state.x
    end,
}
