class Player {
	// private members
	#connectiontimer = undefined;
	#positiontimer = undefined;
	#rpc = undefined;
	#player = undefined;
	#timeout = 10000;
	maxitems = 5;
	// virtual methods
	/*
	changestate = undefined;
	changeduration = undefined;
	changeinfo = undefined;
	changeoptions = undefined;
	changenext = undefined;
	changevolume = undefined;
	changelist = undefined;
	setevents = undefined;
	setactions = undefined;
	*/
	constructor(player, maxitems, timeout) {
		this.#player = player;
		this.#rpc = new JsonRPC(this.#player.url);
		this.#rpc.onopen = this.#onopen.bind(this);
		this.#rpc.onclose = this.#onclose.bind(this);
		this.#rpc.onchange = this.#onchange.bind(this);
		this.#rpc.capabilities = this.#capabilitiescb.bind(this);
		if (maxitems)
			this.maxitems = maxitems;
		if (timeout)
			this.#timeout = timeout * 1000;
		this.#rpc.connect();
	}
	close() {
		this.#rpc.close();
	}
	changeplayer(player) {
		this.#rpc.close();
		this.#player = player;
		this.#rpc = new JsonRPC(this.#player.url);
		this.#rpc.connect();
	}
	#onopen() {
		if (this.#connectiontimer != undefined)
			clearInterval(this.#connectiontimer);
		this.load();
	}
	#onclose() {
		if(this.changestate) {
			this.changestate(-1, 'disconnect');
		}
		if (typeof this.onclose === "function")
			this.onclose();
		if (this.#connectiontimer != undefined)
			clearInterval(this.#connectiontimer);
		this.#connectiontimer = setInterval(function() {
			this.#rpc.connect();
		}.bind(this), this.#timeout);
	}
	#onchange(result) {
		if (result.state && this.changestate) {
			this.changestate(result.id, result.state);
		}
		if (result.duration && this.changeduration) {
			this.#rpc.getposition = this.#getpositioncb.bind(this);
			this.duration = result.duration;
			if (this.#positiontimer != undefined)
				clearInterval(this.#positiontimer);
			this.changeduration(result.id, 0, result.duration);
			this.#positiontimer = setInterval(function() {
				this.#rpc.send('getposition', null);
			}, 1000);
		}
		else if (this.#positiontimer != undefined && this.changeduration) {
			this.changeduration(result.id, 0, 0);
			clearInterval(this.#positiontimer);
			this.#positiontimer = undefined;
		}
		if (result.media && this.changemedia) {
			this.changemedia(result.id, result.media);
		}
		if (result.info && this.changeinfo) {
			this.changeinfo(result.id, result.info);
		}
		if (result.options && this.changeoptions) {
			this.changeoptions(result.id, result.options);
		}
		if (result.id && result.id > -1 && this.#rpc.list) {
			var firstid = result.id - (result.id % this.maxitems);
			this.#rpc.send("list", {maxitems:this.maxitems, first:firstid});			
		}
		if (result.next && result.next != -1 && this.changenext) {
			this.changenext(result.next);
		}
		if (result.volume && this.changevolume) {
			this.changevolume(result.level)
		}
	}
	#statecb = function(result) {
		this.changestate(result.id, result.state);
	}
	#status(result) {
		this.#onchange(result);
	}
	#getpositioncb(result) {
		if (result.position >= 0)
		{
			this.changeduration(0, result.position, result.duration);
		}
		else if (this.positiontimer != undefined)
		{
			this.changeduration(-1, 0, 0);
			clearInterval(this.positiontimer);
			delete(this.positiontimer);
			this.positiontimer = undefined;
		}
	}
	#filtercb(result) {
		if (this.#rpc.list)
			this.#rpc.send("list", {maxitems:this.maxitems, first:0});
	}
	#infocb(result) {
		if (result.info && typeof this.changeinfo === "function")
			this.changeinfo(result.id, result.info);
		else if (result.message)
			console.log(result.message);
	}
	#listcb(result) {
		this.changelist(result.playlist, result.count);
	}
	#optionscb(result) {
		let options = [];
		for (let option of Object.keys(result)) {
			if (result[option])
				options.push(option);
			else
				options.push("!"+option);
		}
		this.changeoptions(result.id, options);
	}
	#nextcb(result) {
		this.changenext(result.next);
	}
	#setinfocb(result) {
		this.saveinfo(result.message);
	}
	#changecb(result) {
		this.#onchange(result.message);
	}
	#volumecb(result) {
		if (typeof this.changevolume === "function")
			this.changevolume(result.level);
	}
	#capabilitiescb(result) {
		for (let event in result.events) {
			switch (result.events[event].method) {
			case "onchange":
				// is too late to set player.onchange here
			break;
			}
		}
		if (this.setevents) {
			this.setevents(result.events);
		}
		for (let action in result.actions) {
			switch (result.actions[action].method) {
			case "pause":
			if (this.changestate) {
				this.#rpc.play = this.#statecb.bind(this);
				this.#rpc.pause = this.#statecb.bind(this);
			}
			break;
			case "stop":
			if (this.changestate) {
				this.#rpc.stop = this.#statecb.bind(this);
			}
			break;
			case "next":
			if (this.changestate) {
				this.#rpc.next = this.#statecb.bind(this);
			}
			break;
			case "change":
				this.#rpc.change = this.#onchange.bind(this);
			break;
			case "setnext":
			if (this.changenext) {
				this.#rpc.setnext = this.#nextcb.bind(this);
			}
			break;
			case "status":
				this.#rpc.status = this.#onchange.bind(this);
				this.#rpc.send("status", null);
			break;
			case "list":
			if (this.changelist) {
				this.#rpc.list = this.#listcb.bind(this);
			}
			break;
			case "options":
			if (this.changeoptions) {
				this.#rpc.options = this.#optionscb.bind(this);
			}
			break;
			case "filter":
			if (this.changelist) {
				this.#rpc.filter = this.#filtercb.bind(this);
			}
			break;
			case "info":
				this.#rpc.info = this.#infocb.bind(this);
			break;
			case "setinfo":
			if (this.saveinfo) {
				this.#rpc.setinfo = this.#setinfocb.bind(this);
			}
			break;
			case "volume":
				this.#rpc.volume = this.#volumecb.bind(this);
			break;
			}
		}
		if (this.setactions) {
			this.setactions(result.actions);
		}
	}
	load() {
		this.#rpc.send('capabilities',null);
	}
	play() {
		if (this.#rpc.play)
			this.#rpc.send('play',null);
	}
	stop() {
		if (this.#rpc.stop)
			this.#rpc.send('stop',null);
	}
	pause() {
		if (this.#rpc.pause)
			this.#rpc.send('pause',null);
	}
	next() {
		if (this.#rpc.next)
			this.#rpc.send('next',null);
	}
	volume(step) {
		if ( !this.#rpc.volume)
			return;
		if (step) {
			this.#rpc.send('volume', {step:step});
		} else {
			this.#rpc.send('volume', null);
		}
	}
	filter(params) {
		if (this.#rpc.filter)
			this.#rpc.send("filter", params);
	}
	change(media) {
		if (media.next != undefined && this.#rpc.setnext) {
			this.#rpc.send("setnext", {id:media.next});
		}
		else if (media.media != undefined && this.#rpc.change) {
			this.#rpc.send("change", media);
		}
	}
	about(id) {
		if (id)
			this.#rpc.send("info", {id:id});
		else
			this.#rpc.send("status", null);
	}
	shuffle(enable) {
		if (this.#rpc.options)
			this.#rpc.send("options", {random:enable});
	}
	repeat(enable) {
		if (this.#rpc.options)
			this.#rpc.send("options", {loop:enable});
	}
	list(params) {
		if ( !this.#rpc.list)
			return;
		if (params == undefined)
			params = {maxitems:this.maxitems, first:0};
		if (! params.maxitems)
			params.maxitems = this.maxitems;
		if (! params.first) {
			if (params.id) {
				params.first = ((params.id / this.maxitems) * this.maxitems) + 1;
			}
			else
				params.first = 0;
		}
		this.#rpc.send("list", params);
	}
};

class PlayerCmd extends Player
{
	#cmdbar = undefined;
	#state = undefined;
	#shuffle = true;
	#repeat = false;
	#cmdaction = undefined;
	#cmdcontrol = {};
	#id = "playercmd";
	constructor(config, maxitems, cmdbar) {
		super(config, maxitems);
		if (typeof cmdbar === "object") {
			this.#cmdbar = cmdbar;
			this.#cmdaction = "update";
			if (cmdbar.id)
				this.#id = cmdbar.id;
		}
		else {
			if (typeof cmdbar === "string")
				this.#id = cmdbar;
			this.#cmdbar = new CmdBarClient(null, this.#id);
		}
		this.#cmdbar.load(null, this.#id, this.listener.bind(this));
		this.load();
	}
	setactions(actions)
	{
		if (this.#cmdbar == undefined)
			return;
		var message = new CmdBarControl(true, this.#cmdaction);
		var buttons_cmd = [];
		var buttons_opt = [];
		var buttons_vol = [];
		for (let action of actions) {
			switch (action.method) {
			case "next":
				buttons_cmd.push({
					id:action.method,
					glyphicon:"fast-forward small",
					name:action.method,
				});
			break;
			case "stop":
				buttons_cmd.push({
					id:action.method,
					glyphicon:"stop small",
					name:action.method,
				});
			break;
			case "pause":
				buttons_cmd.push({
					id:"playpause",
					glyphicon:"play small",
					name:"toggle play pause",
				});
			break;
			case "options":
				for (let option of action.params)
				{
					if (option == "random")
					{
						buttons_opt.push({
							id:"shuffle",
							glyphicon:"random small",
							name:"toggle shuffle",
							click: "toggleshuffle(!$(this).hasClass(\'btn-primary\'));",
						});
						message.addscript("toggleshuffle", undefined, PlayerCmd.toggleshuffle);
					}
					if (option == "loop")
					{
						buttons_opt.push({
							id:"repeat",
							glyphicon:"repeat small",
							name:"toggle repeat",
							click: "togglerepeat(!$(this).hasClass(\'btn-primary\'));",
						});
						message.addscript("togglerepeat", undefined, PlayerCmd.togglerepeat);
					}
				}
			break;
			case "volume":
				buttons_vol.push({
						id:"volume-down",
						glyphicon:"volume-down small",
						name:"volume down",
					});
				buttons_vol.push({
						id:"volume-up",
						glyphicon:"volume-up small",
						name:"volume up",
					});
			break;
			case "info":
			case "play":
			case "status":
			case "change":
			case "list":
			case "setnext":
			case "getposition":
			case "filter":
			case "append":
			case "remove":
			break;
			default:
				console.log("unknown action "+action.method);
			}
		}
		message.addgroup(buttons_cmd);
		message.addgroup(buttons_opt);
		message.addgroup(buttons_vol);
		this.#cmdcontrol = message;
		this.#cmdbar.load(message, this.#id);
		this.#cmdaction = "refresh";
	}
	changeoptions(id, options) {
		var cmdbar = {
			buttons: [],
		};
		for (const option of options) {
			var button = {};
			switch (option) {
			case "random":
			case "!random":
				button.id = "shuffle";
			break;
			case "loop":
			case "!loop":
				button.id = "repeat";
			break;
			}
			if (option[0] === "!")
				button.classes = '!btn-primary';
			else
				button.classes = 'btn-primary';
			cmdbar.buttons.push(button);
		}
		var message = new CmdBarControl(true,"refresh");
		message.addgroup(cmdbar.buttons);
		this.#cmdbar.load(message, this.#id);
	}
	changestate(id, state) {
		this.#state = state;
		var action = "refresh";
		var cmdbar = {
			buttons: [],
		};
		var menu = undefined;
		switch (state){
		case "play":
			var button = {
				id:"playpause",
				glyphicon: "pause  small",
				};
			cmdbar.buttons.push(button);
		break;
		case "disconnect":
			action = "remove";
			break;
		case "pause":
		case "stop":
		default:
			var button = {
				id:"playpause",
				glyphicon: "play  small",
				};
			cmdbar.buttons.push(button);
		break;
		}
		var message = new CmdBarControl(true, action);
		message.addgroup(cmdbar.buttons);
		if (menu)
			message.addmenu(menu);
		this.#cmdbar.load(message, this.#id);
	}
	changeinfo(id, info) {}
	changeduration(id, position, duration) {}
	static toggleshuffle(message) {
		if (message == false) {
			send2app('random off');
		}
		else {
			send2app('random on');
		}
	}
	static togglerepeat(message) {
		if (message == false) {
			send2app('repeat off');
		}
		else {
			send2app('repeat on');
		}
	}
	listener(message) {
		//console.log("cmdbar server message "+ message);
		switch (message) {
		case "toggle play pause":
			if (this.#state == "play")
				this.pause();
			else
				this.play();
		break;
		case "stop":
			this.stop();
		break;
		case "next":
			this.next();
		break;
		case "random on":
			this.shuffle(true);
		break;
		case "random off":
			this.shuffle(false);
		break;
		case "repeat on":
			this.repeat(true);
		break;
		case "repeat off":
			this.repeat(false);
		break;
		case "volume down":
			this.volume(-5);
		break;
		case "volume up":
			this.volume(+5);
		break;
		}
	}
	onclose() {
		var message = new CmdBarControl(true, "remove");
		message = Object.assign(this.#cmdcontrol, message);
		this.#cmdbar.load(message, this.#id);
	}
};
