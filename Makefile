BUILD ?= obj

CFLAGS += -g -O3 -mrtm -D_GNU_SOURCE=1
LDFLAGS += -no-pie

all: call_rdrand crosstalk get_root
.PHONY: clean

call_rdrand-obj = $(addprefix ${BUILD}/, src/call_rdrand_core0.o)
get_root-obj = $(addprefix ${BUILD}/, src/get_root.o)

crosstalk-obj = $(addprefix ${BUILD}/, src/leaker_core1.o)
fpvi-obj = $(addprefix ${BUILD}/, src/fpvi.o)

deps += $(call_rdrand-obj:.o=.d)
deps += $(get_root-obj:.o=.d)

deps += $(crosstalk-obj:.o=.d)
deps += $(fpvi-obj:.o=.d)

-include ${deps}

call_rdrand: ${call_rdrand-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${call_rdrand-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

get_root: ${get_root-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${get_root-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

crosstalk: ${crosstalk-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${crosstalk-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

fpvi: ${fpvi-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${fpvi-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

$(BUILD)/%.o: %.c
	@echo "CC $<"
	@mkdir -p "$(dir $@)"
	@${CC} -c $< -o $@ ${CFLAGS} -MT $@ -MMD -MP -MF $(@:.o=.d)

clean:
	@rm -f call_rdrand get_root crosstalk fpvi
