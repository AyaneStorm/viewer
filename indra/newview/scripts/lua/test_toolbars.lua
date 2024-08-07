popup = require 'popup'
UI = require 'UI'

local OK = 'OK_okcancelbuttons'
local BUTTONS = UI.getToolbarBtnNames()
local TOOLBARS = {'left','right','bottom'}

-- Clear the toolbars and then add the toolbar buttons to the random toolbar
response = popup:alertYesCancel('Toolbars will be randomly reshuffled. Proceed?')
if next(response) == OK then
    UI.clearAllToolbars()
    math.randomseed(os.time())

    -- add the buttons to the random toolbar
    for i = 1, #BUTTONS do
      UI.addToolbarBtn(BUTTONS[i], TOOLBARS[math.random(3)])
    end

    -- remove some of the added buttons from the toolbars
    for i = 1, #BUTTONS do
      if math.random(100) < 30 then
        UI.removeToolbarBtn(BUTTONS[i])
      end
    end
    popup:tip('Toolbars were reshuffled')
else
    popup:tip('Canceled')
end
